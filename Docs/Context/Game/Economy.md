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
| Lumberyard | the TREES in the radius, each by its SIZE / "Trees for full lumberyard yield" (3). A tree counts (its size / the world's average tree) ^ "Harvest size exponent" (2): its size is its trunk at the ground (the species' trunk x the record's own scale, `TreeWorld::surveyHarvest`, the expansion's scale from the same seed) over the average trunk of the world's tree species, so an average tree is 1, a small one ~0.4, a big one 2 and more. Dead wood (a log, a stump) is size 0.7 at its type's mean length (half a tree), by its length; a bush counts nothing. |
| Quarry | the ROCKS in the radius, each (its longest axis / "Harvest reference rock (m)" (3)) ^ "Harvest size exponent" (2), / "Rocks for full quarry yield" (4): a 3 m rock is 1, a 6 m boulder alone is a full yield (4), a 20 m monolith 44 (it feeds many quarries: they share it). A cactus counts nothing. |
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
| A hub's feed | Every CONSUMER (fields, the turret, the barracks, the medic station) links to the nearest hub within "Hub reach (m)" (30) with a clear line, and fills from it at "Feed rate" (10/s). **THE LIGHT BALANCE** (defaults; the default sun cycle gives a Mirror ~780 s of full daylight and keeps a field awake ~640 s a night; all with enough storage between): an Emitter draws 0.75 light/s awake (480 a night), a Bastion 2 (1280 a night). **2 Mirrors carry 1 Emitter through the night, 5 a Bastion** ("Solar energy/s" 0.33: 2 x 0.33 x 780 = 515). **1 Hearth, burning day and night, carries 3 Emitters or 1 Bastion** ("Gen energy/s" 1: 1 x 1440 = 1440). **ONE full Crystal store ("Battery capacity" 1500) runs TWO Emitters through one night** (960, plus room for the pressure draw). A change of the sun cycle's speed or latitude moves all of these. **The barracks and the turret keep their METERED intake** (their store is the build / reload bar). The Beacon keeps a quarter of its store for its own field. |
| Hub to hub | **THE HUB NETWORKS** (`rebuildLightLinks`): a SPANNING FOREST (Kruskal) over every hub pair of one team in reach - "Hub link range (m)" (60), a pair with a Lens end "Lens range (m)" (32) - shortest first; a pair joins two networks not yet joined if its line is clear (a ray only for such a pair). So every hub that reaches another by a chain of links is in ONE network, with one link less than it has hubs (the beams). Each network is a tree in breadth-first order from its root (the Beacon when it is in it): `m_lightNet`, and each hub's `lightId` is its parent. (Nearest-hub links made islands: two pairs of stores close together each linked inside the pair, and their light never met.) **A network levels as ONE POOL** (`levelLightNet`, every tick): the network's light is shared out by the **store PRIORITY** (Balance / Fill / Drain, see Store priority in [Hotbar.md](Hotbar.md)) - the Fill hubs to one fill fraction first, then the Balance hubs, then the Drain hubs; the Beacon's reserve (a quarter) is locked out of the pool. Every hub moves toward its share at "Share rate" (10/s; a Lens at "Lens rate", 60/s), the takers' and the givers' totals scaled to the same amount (no light made or lost). A beam's flow is what the child's whole subtree took or gave (`m_lightPoolMoved`), so the beams show where the light really goes. |
| **The Lens** | A RELAY (`lens.pre`, 1×1, a 3 m post; the Resource page): a hub with a small store ("Lens buffer" 40). Nothing is aimed: it is a hub of the networks above, and a pair with a lens end is in reach within "Lens range (m)" (32). **A line of lenses 32 m apart, or less, is a chain** that joins its two ends into one network. A link with a lens at an end is never longer than the lens range, whatever the other end's own range (a store's 60 m link range, a mirror's beam range). A lens moves toward its share at "Lens rate (light/s)" (60; not the share rate), so light goes both ways: from a far mirror field to the base, or from the base to a far outpost. As a hub, a lens also takes the mirrors' and hearths' light and feeds the consumers in its reach. **That rate is their total:** the lens's store is all its own producers fill and all its own consumers take, and the pool refills or empties it at most at "Lens rate" - so a lens at the end of a chain feeds its towers at most 60 light/s together, however many crystals stand behind it. Light that only passes through a lens is not capped by it (a beam carries the whole subtree's move). The ghost shows the lens range. Like a Crystal store, a lens never shows the light badges: the end of a chain has no link of its own (the others link to it). |
| Which hub | **The nearest USEFUL hub in range with a clear line, else the nearest one** (`rebuildLightLinks`). Useful: for a consumer a hub with light to give (over the Beacon's reserve), for a producer a hub with room. So a turret or a barracks next to a full Crystal store does not stay dark because an empty hub is nearer (the Beacon at its reserve, a lens with nothing behind it), and a mirror does not beam into a full store. The links are derived again about once a second, so a link follows the light. Hub to hub: the networks above. |
| A beam's ends | `beamPointOf`: the CENTRE OF THE BUILDING'S CRYSTAL (`beamHeightOf` in Light.cpp, per type; keep it in step with the `Crystal` child of the .pre files); a building with no crystal: 1 m over its top. |
| Shown while building | With a type that uses light on the ghost, `drawDebug` (its `armed` input) rings each built hub in gold at the distance that type links over: the same range choice as `rebuildLightLinks`, a Lens end never longer than "Lens range". |
| A clear line | `lightLineClear`: one physics ray between the two beam points against the Terrain and Vegetation layers. Buildings and units do not block. |
| When | `rebuildLightLinks` about once a second, and at once when the roster size changes (`tickLight` on the authority, `tickMirror` on a client: the beams and the badges). `tickLight` (authority, after the carriers) moves the light. |
| Drawn | `drawLightLinks`, called from `tickAuthority` / `tickMirror` (**never from `drawDebug`: that runs before the begin-frame job, which resets the frame's light records**): **a link shows only while it MOVES light** - `Ref::lightFlow` above 0.02 light/s, the link's MEASURED flow, smoothed over ~1/3 s in `tickLight` (a store that is topped up every tick never looks busy, so the stores cannot say it); on a mirror it is an estimate from what the building makes or draws - as a `BeamLight` between the two crystals ("Beam glow", "Beam radius (m)", "Beam glow range (m)"; **the glow is × flow / "Beam full flow (light/s)" (20, the "Beam rate"), and the radius goes from half to full with it**, so a beam shows how much light it carries): the Beacon's beam in small. **A MIRROR's beam is LIGHT GLITTER, not a beam column** (within 400 m of the camera): golden sparks that fly from the mirror to its hub's crystal - `Effects/light_glitter.pfx` through `ParticleSystem::emitStream` (Particle's CONTEXT, "Streams"), "Mirror glitter (sparks/s)" (14) x the mirror's share of its full output. A field of mirrors is dozens of small beams; a stream moves a few emitter slots from beam to beam, and it is no light. Tried debug lines, dropped because a 1 px line has no glow. **Every other moving link's glow is a BEAM CARD** (`Renderer::addBeamCard`, BeamCardPipeline in RendererVK's [Lighting.md](../RendererVK/Lighting.md)): the fog apply's column glow on a quad around the beam, so it costs the pixels it covers, not the whole screen - up to 256 a frame, the best first. **The light on surfaces only for the "Lit beams" (10) best** (`addLightInfo(beam, false)`: no air column): the score is (0.25 + the flow share) x (1 + length / 20 m) / the distance from the camera to the beam's nearest point (at least 5 m), x 1.3 for a link that is lit now (no flicker at the edge of the list). Each link fades its light in and out over 0.5 s (`Ref::beamLit`); the glow in the air never changes, so the switch does not show. Tried particle streams for the links past the column count, dropped because they looked too unlike the beams. No line is drawn. The cards draw with the fog off too. |
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

**Light runs the fields and the machines of defence** (emitters, the turret, the barracks, the medic station, the wards).
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

### Carriers: wood, stone and munitions on the road

**With "Game/Carriers" "Enabled" (the default) only ENERGY rides the cable transport below.** Fuel (wood),
minerals (stone) and MUNITIONS (`GoodMunitions`, store index 3 - no cable carries them at all) move by WALKERS (`Carriers.cpp`, `StructureSystem::tickCarriers`, authority, after the
constructors). `transportInject` skips every slot of medium 1 and 2, so those cables hold and move nothing. The
pipeline is off the hotbar; the conveyor is the **Road**.

| | |
|---|---|
| The look | `cableConveyor.pre` + `Models/roads.obj` (hand-written): cobblestones, 5 cm high with a bevel, a road 1.2 m wide. The hub is the centre patch of stones (`RoadHub`); each of the four arm children is a strip of stones from the patch to the cell edge (`RoadArmX` / `RoadArmZ`, centred meshes moved to ±0.83, no rotation), shown toward what the segment joins. So an end, a straight, a corner, a T and a crossing each get their shape, and two neighbour cells' strips meet in one joint on the cell edge. |
| The road | BUILT `CableConveyor` segments and `CrossingConveyor` pieces (`roadAt`: the cell's occupant or the cable under a crossing). A blueprint is no road. **A built GATE with road on two opposite sides** (+X and -X, or +Z and -Z) is road too, at the middle height of the two (its sides are tested without the gate rule, so two gates side by side do not ask each other): the walkers pass through a wall's gate. **A LANDED SHIP's free deck tiles are road too** (at the deck top), so the buildings on its deck are on the road and a road up to an entrance in its rail joins the deck ([Ship.md](Ship.md)). |
| Who walks | A walker (`m_walkers`, a `Walker`) is one FREE WORKER of its team: workers - production jobs (see Population). It comes out of a House or the Beacon that touches a road (`touchesRoad`), at most "House workers" / "Base workers" per building. The HUD's "Workers" reads `jobs + walkers / workers`. |
| The job | A TRANSPORT JOB is a building that SENDS goods (`carrierSendGood`, by `transportRoleOf`): a PRODUCER sends when its store holds a load ("Load", 10) or half its capacity; a STORAGE (Storehouse, Base) sends a load only to a CONSUMER that is under half full. A walker goes to the job's road cell (the PICKUP) and STAYS: load, deliver, come back, wait there. |
| Who gets a walker | `carrierJobTier`: 2 = the building has a load to send (`Ref::sendReady`) and no walker waits at it or is on its way (so a long road gets MORE walkers, up to "Max walkers per building" 6); 1 = a producer with no walker at all, or build work (below). `roadSearch` takes the nearest tier 2, else the nearest tier 1. |
| The ~1 s pass | `carrierHousePass`: derives `sendReady` (a storage: only with a hungry consumer on its road), then every house sends ONE worker per pass while a job wants one and the team has a free worker. When production takes the worker (free < walkers), the house DISMISSES walkers, the waiting ones first: a dismissed walker goes home when it is next idle. |
| Waiting | The first walker in line at a pickup (`Ref::waitRank`) loads; it looks for a load about once a second. A walker BEHIND another one looks for other work on its road network (tier 1 or 2). The first in line leaves only for a waiting load elsewhere (tier 2) and only after 8 s idle. After 20 s idle a walker goes home, except the one walker of a producer. |
| Where to | `findRoadTarget` = `roadSearch` with the takers as the goal: ONE breadth-first search over the road cells from the walker's cell (4096 cells at most). **The first TAKER with room that a road cell touches wins - a storage as much as a consumer**, so a load stops at the nearest storehouse and that storehouse's own carriers feed the consumers farther on (a consumer first, whatever the distance, sent every load past a near storehouse to a far one). Same team, built. **A taker counts only when the WHOLE load fits** (`carrierAccepts`): the load the source would send against the taker's room, less what other walkers already carry to it (`Ref::goodsIncoming`, counted fresh each tick, every good) - a hearth that misses one wood is no target for a full load. **MUNITIONS are the exception**: whole ones, a load as big as the room (at least one), so a cannon is topped up and not only refilled when empty. That is why the buffers hold several loads ("Internal buffer", "Generator fuel tank", "Generator buffer": 30). The result is also the walking path (cell centres). |
| BUILD WORK | **A walker with no other load BUILDS**: it carries stone to the BLUEPRINTS on the road - buildings, and pieces of the road itself. `blueprintNeed` = the stone a blueprint still needs (`investMaterials`' price) less what the walkers already carry to it (`Ref::buildIncoming`, counted fresh each tick). The pass sets `Ref::buildReady` on a building that holds stone and is no consumer (`carrierHoldsStone`: a store, the Beacon, a quarry) with a blueprint on its road; that is tier 1 in `carrierJobTier`, so a waiting LOAD (tier 2) always gets the walker first. At the pickup the first in line tries the loads, and only then takes a load of stone to the nearest blueprint (`findRoadTarget` with `blueprints`: `carrierAccepts` then takes ONLY blueprints, never one otherwise). At the blueprint it STAYS AND WORKS (`Walker_Working`): its stone goes in through `investMaterials` at "Build rate (stone/s)" (2), until the load is used up or the blueprint takes no more (built, gone, or rubble with an enemy near); `buildIncoming` counts its load during the work too. What is left goes on to the NEXT blueprint from there (a road piece that was just built is road for that search, so one walker lays a painted road piece by piece), and the rest goes back to the pickup by a new search from there. A blueprint is found only when a BUILT road cell touches it: the first piece next to a building is the player's or the Builder's hut's. |
| REPAIR WORK | **The last work of all**: the same trip, to a DAMAGED built structure at the road (`repairNeed` over 0.25 stone; `ECarrierWork`: Goods, Build, Repair - `Walker::work` says what a load is for). `Ref::repairReady` is set only while the team has NO build work on any road (it shares the build work's tier 1, so that gate puts it behind), and at the pickup a walker tries the loads, then a blueprint, then a repair. The walker works at the structure like at a blueprint, and a repair also has a health rate: the Builder's hut's "Constructor repair health/s" in the ratio of the two stone rates (20 x 2 / 4 = 10 health/s). What is left goes on to the next damaged structure. |
| TRANSPORT (the cart) | **Behind every other work of the building**: a CART load ("Cart load", 100) from one STORAGE to another one of the same good, and between storages only (`transportRoleOf` Storage on both ends: wood between Storehouses and Wood stores, stone between Storehouses and the Beacon) - it evens out the fill of the stores of a base. `transportAmount(source, target, good)`: the fills (store over capacity; the target's with `goodsIncoming`) must differ by 0.15 or more (a Storehouse holds 1000 wood and 1000 stone: "Fuel tank capacity", "Mineral silo capacity"); the load is what makes the two fills equal, at most a cart, at most what the source holds and the target has room for; under 0.2 of a cart there is no trip. **Two stores of different PRIORITY** (Store priority in [Hotbar.md](Hotbar.md)): the cart goes only from the lower rank to the higher one and takes all it can, whatever the fills. `carrierAccepts` with `CarrierWork_Transport` is that test, so `findRoadTarget` gives the NEAREST such storage by road. The pass sets `Ref::transportReady` on a storage with no load, build or repair work (only when the team has two storages of the good); it is tier 1 like build work. At the pickup the first in line tries the loads, a blueprint, a repair, then the cart. The trip is a goods trip: unload what fits, bring the rest back. |
| The road is gone | Every tick, a walker whose road cell is no road any more (destroyed to rubble, removed) moves to a road cell NEXT to it. A standing one (waiting, working, guarding, crewing) stands there. A walking one is put on that cell and finds a new way to where it was going (its job, its target, its home; a guard: a new post; a crew: its gun or its munitions); with no way there it goes home. With no road cell next to it the walker is removed, and its house sends a worker again at the next pass. |
| The trip | The load leaves the store at departure. The walker goes at "Walk speed (m/s)" (3.5), unloads what fits, walks the same path back to the pickup, and puts the rest back into the job's store. |
| The walker | `carrier.pre`: render-only, `Global true`, no physics and no unit sim; the tick writes its `pos` / `rot` (it faces its walking direction: an entity's forward is -Z) and shows its LOAD (the prefab's second child, held in front of the body) only while it carries one on foot. On a transport trip it pulls the CART (the children from the fourth on; the third is the cart's load, shown while the cart is full), out and back. Removed when it arrives home, when its house is gone, or when no road leads home. The per-building counters (`Ref::walkers`, `walkersIdle`, `homeWalkers`) are counted from `m_walkers` at the top of every tick, so a removed building needs no bookkeeping. |
| Badge | "No road": a building that moves wood or stone and touches no road (`attachedMask` bit 2), or a House that touches none (its workers cannot go out). |

So the delivery RATE of ONE walker is load x speed / (2 x road length), and a job takes walkers until its loads no
longer wait: throughput holds over a long road, for the price of workers. **Known gaps:** the walkers exist on
the authority only (clients see the stores change, and their HUD shows no walkers), a save does not hold a walker (a
load on the road at F9 is lost; the walkers come out again), and demons do not attack a walker.

### The Cannon and munitions

**MUNITIONS are the fourth good** (`GoodMunitions` = 3: `GameStructureComponent::store[3]`, `NumStores` 4). Light charges
for the cannons: made by the Ammo works, banked by the Ammo store, used by the Cannon (`transportRoleOf` medium 3:
Producer / Storage / Consumer). They move ONLY by carrier (every goods loop in Carriers.cpp runs 1..3); the cable
transport never sees index 3. A carrier with munitions holds a pale crystal (`carrier.pre`'s eighth child) instead of
its pack. Capacities: `munitionCapacityOf`. Synced in GSt, saved as `Munitions`.

| Building | Rules |
|---|---|
| **Ammo works** (`ammoWorks.pre`, 2x2, the Production page) | Packs stone (by carrier: a stone consumer, "Ammo works buffer" 50) and light (its light link, like a consumer: "Internal buffer") into munitions in its own buffer (50) at "Ammo works munitions/s" (0.5) x staffing, "Stone per munition" (1) and "Light per munition" (1); short of either it makes what they pay for (`tickProduction`). Needs "Ammo works workers" (2). `powered` = it made munitions this tick (or it is full). Badges: no light store, no road, short of workers. |
| **Ammo store** (`ammoStore.pre`, 1x1, the Resource page) | Banks "Ammo store capacity" (30). A storage of munitions: the carts level the stores, the priority popup works on it (`hasStorePriority`). **DESTROYED WITH MUNITIONS INSIDE IT EXPLODES** (`ammoBlast`, in the death sweep, before its stores are cleared): radius "Blast radius (m)" (8) x (0.4 + 0.6 sqrt(fill)), damage "Blast damage" (400) x (0.25 + 0.75 fill), falling to half at the edge, to units of EVERY team (players through their damage inbox) and every building in it (its own team's too: another store in it can go up next). The match shows the flash (`blasts()`: `Effects/ammo_blast.pfx` puffs + sounds; authority only). |
| **Cannon** (`cannon.pre`, 1x1, the Emplacement page) | **One cell, 2.2 m tall (a field gun of boxes: a square bronze barrel on a wooden carriage with two big square wheels and a trail); its square mouth reaches ~0.6 m past the cell in front, ~1.9 m over the stand**, so on a walkway deck behind a wall, or on the ship's rail, it fires over the wall. **It may stand on a road piece (the Road, the Bridge), a walkway DECK or a SHIP RAIL tile** (on the deck: `deckCellsFree` lets it share the rail's tile, and its turn on the deck is `Ref::shipTurn`, eighths relative to the ship, saved as `ShipTurn`; it is no part of the ship's collider; on the FLYING ship no crew reaches it, so it does not fire) (`cellsFree`: the cell's primary occupant stays the piece, the cannon rides `underId`, so the road stays road; `cannonStandY` gives the stand height, also for the ghost and the load), or on free ground. **WALK-THROUGH** (`isWalkThrough`: no Nav obstacle; its collider is Layer Cable vs Projectile, like the Mirror). It touches the road through its own cell too (`touchesRoad`), and the walkers' road search looks at both occupants of a cell, so it is found as a delivery target ON the road. **Placed TURNED** (`isRotatedPlacement`): R turns the ghost in eighths of a turn; `placeStructure` quantizes the facing; saved as `Facing`. Holds "Munitions held" (5; the carriers top it up in whole munitions, `carrierAccepts`). |
| **The gun turn** | The whole model turns: the entity's rotation is the placed facing turned by `Ref::cannonAim` (radians about +Y, within the arc). `cannonFacingRot` undoes the turn wherever the FACING is meant (the arc, the save's `Facing`, `structureFacing` for the GPl replay - so a client spawns it facing the right way); `tickShipStructures` puts the turn on a ship cannon's rotation. `turnCannon` turns it toward `Ref::cannonAimGoal` at "Turn rate (deg/s)" (60). A client gets the turn in GSt's OUTPUT byte (no emitter on a cannon: 0..255 = -pi..pi) and its `tickMirror` turns the gun toward it at the same rate. |
| **The crew** (Carriers.cpp) | **A cannon fires only when MANNED**: a walker of its crew at the gun (`Ref::crewAtGun`). Its ALARM (`Ref::alarm`, the ~1 s pass): an enemy unit within "Crew call range (m)" (60), held for "Crew stand down (s)" (15). An alarmed cannon with fewer than "Crew" (2) walkers (`crewWanted`) calls FREE workers like an Armory calls guards: a House sends one (after a guard, before the carrier jobs), and a waiting walker goes (after the Armory). The crew walks to the road cell next to the gun (`Walker_CrewToCannon`), then stands AT THE GUN (`Walker_Crewing`): one on each side, just outside the wheels (1.05 m) and 0.35 m behind the axle, on the gun's stand (a third and fourth one 0.7 m behind those), turning with the gun and looking where it points. (Its road cell stays the walk's end.) **KEEPING IT ARMED** (every 0.5 s at the gun): while no other one of the crew is on a trip (`Ref::crewFetching`) and the gun has room for one more munition (with what is on its way), one walks to the nearest Ammo store / Ammo works holding one by road (`crewAmmoSource`; `Walker_CrewFetch`), takes as many as the gun has room for (at most a load) and brings them (`Walker_CrewBring`, counted in `goodsIncoming`). It goes only while another one stays at the gun, or when the gun is EMPTY: a lone crew fires what it has first. The alarm is over, the gun is gone or rubble, or production took the worker: it takes other work or goes home (a trip under way finishes first). The crew counters are counted from `m_walkers` every tick like the guards. With the carriers off there is no crew and every cannon is manned. |
| **Firing** (`tickCannons`, authority, after the constructors) | Manned, a cannon RELOADS at "Fire interval (s)" (3) x "Crew" / the crew at the gun (one of two at the gun: half the rate). Every 0.2 s it takes the NEAREST enemy unit - a SHADOW too - within "Arc (deg)" (150) / 2 of its placed facing, between "Min range (m)" (10) and "Range (m)" (45), planar, **that a lob can reach**: of the 4 nearest, the first whose HIGHEST lob (the clearance check's last try) has a clear arc; none = no target, the gun holds rather than fire into its own wall. It sets the gun's aim goal to its bearing. It FIRES when the gun points at it (within ~3 degrees), the reload is done and it holds a munition: `cannonShell.pre` from the muzzle of the gun as it points (1.65 m ahead, 0.55 m over the origin) as a LOB at the target's place of the last search: flight time = the distance / "Shot speed (m/s)" (20), at least 1 s, and at least long enough for a launch 45 degrees up (a cannon on a walkway deck has its muzzle only ~0.65 m over the 3 m wall in front), the vertical speed solved for the gravity so it comes down on that spot (no lead). **The start:** the shell begins 1 m ALONG ITS OWN ARC from the mouth (that place and the speed it has there; the same arc), so the wall or the ship's rail right in front of the barrel is behind it already. **The muzzle flash** shows there, on every role (`MuzzleFlash` on `cannonShell.pre`, as big as the impact's burst: `GameProjectileComponent::spawn` emits three `Effects/muzzle_flash.pfx` puffs, the impact's `shell_flash.pfx` and `muzzle_smoke.pfx`, and its `update` adds a cyan point light, 12 m, for 0.15 s, fading). The shell carries its own cyan point light in flight (`Component Light`, intensity 50, 8 m: at the crystal's centre, with ray-traced shadows - the crystal is an emissive tint, so it is out of ray tracing and does not block its own light.). **The clearance check** (`planLob`): from that start, the WHOLE arc to 1.5 m of path before it lands (the shell's lowest point, its radius under the path; 12 rays) against the STATIC world (Default, Gate, Terrain layers: walls, gates, buildings, the ground) - the wall in front of the barrel AND the wall in front of a demon that presses against it; blocked, the flight is 1.3 x longer (a higher lob that comes down steeper), up to 4 times. The lowest clear lob fires; none clear (the target moved behind a wall since the search) = the shot is held and the search runs again at once. Unmanned, the gun stays where it points. **A demon's bubble bends the shell** (`FieldDeflect` 10 on `cannonShell.pre`, `GameProjectileComponent::update`): sideways out of the bubble, down the baked field's planar gradient, at up to 10 m/s^2 by the depth in the field (a smoothstep to the iso value), so a shell lobbed at a Bulwark, an Aegis or a Titan lands a little off. The bake is one slice at one height, so a shell high over a small bubble is bent too. **Bending it costs the bubble** (`DeflectDrain` 40): energy/s x the same depth from the battery of the NEAREST enemy unit whose live bubble covers the shell (planar, its equilibrium radius, searched 16 m around), through that unit's atomic inbox (`GameUnitComponent::drainShield`, applied in its `tickField`). The shell (`NpcSystem::fireCannonShell`, a replicated projectile) splashes every enemy unit within its `SplashRadius` (4) on any contact: 35 at the centre, falling linearly to 15 % of it at the edge (`SplashEdge 0.15`), **a shadow too** (`HitsShadows`: `GameUnitComponent::damage`'s `throughShadow` banks it in `pendingShadowDamage`, which the unit takes whether it is solid or not), and BURSTS (`ImpactPuff`: `Effects/shell_blast.pfx` shards + `shell_flash.pfx`, puff streams GameMatch makes into `GameProjectileComponent::params`; on every role, in the projectile's `destroy`), with a bright cyan point light (intensity 500, 10.8 m, 1 m over the impact, fading out in 0.3 s): `destroy` records the impact, `GameProjectileComponent::drawImpactLights` (GameMatch::update, every role) draws it. |

**Known gaps:** a cannon on a walkway stays where it was placed if the walkway under it turns into stairs or goes away;
the shell has no lead (a running target is missed); the Ammo store blast's look is the authority's only; a crew's
munitions on their way to a gun that is gone are lost.

### The Armory: guards

**The Armory** (`EStructureType::Armory`, `armory.pre`, 2x2, the Unit page, "Game/Armory" tweaks) turns free
workers into GUARDS while enemies are near. A guard is a WALKER (`Carriers.cpp`, the three `Walker_Guard*` states,
its job = the Armory), so everything above holds: authority only, not in a save, nothing hurts it. **THE BEACON IS AN
ARMORY TOO** (`armsGuards` in Carriers.cpp: the Armory and the Base): it has the same alarm, the same range and as
many guards as one Armory, so a base has guards before the first Armory is built. "The Armory" below is either one.

| Step | Rule |
|---|---|
| The alarm | The ~1 s pass: a living enemy unit within "Range (m)" (40, flat) of a built Armory (`nearestEnemyUnit`) sets `Ref::alarm` to "Stand down after (s)" (10; the tick counts it down) and `Ref::threat` to that unit's place. |
| Who goes | `guardWanted`: an alarmed Armory with fewer than "Guards" (6) guards (`Ref::guards`, counted fresh each tick: guarding, or on the way). The HOUSES send a worker to it BEFORE any carrier job, and a WAITING walker goes at its next ~1 s search, before any load (`seekArmory`). Both by `roadSearch`, so the Armory must be on the walker's road. A walker on a trip ends its trip first. |
| The bow | At the Armory the walker's entity is replaced by `guard.pre` (`setGuardLook`, `Walker::guard`). |
| The post | `guardPost`: one breadth-first search over the road from the guard's cell. A post is a road cell within **"Post range (m)" (25, flat) of the Armory** that no other guard has (shorter than the alarm's "Range (m)": the alarm calls the guards early, but each Armory guards its own part of the base, so one wave does not pull every guard to its side); **a WALKWAY DECK cell first** (`Ref::walkDir` -1: the place behind a wall - the player sets the posts by where the Walkway is) **or a built RAIL tile of the LANDED SHIP** (`guardDeckAt`: on the ship's wall; the rest of its deck is plain road) **or the top of a built WARD** (`wardAt`), then any road cell; among those the one nearest to the threat. No free cell: it guards at the Armory. |
| Shooting | `Walker_Guarding`: every "Guard shot interval (s)" (1.2) the nearest living enemy unit within "Guard range (m)" (20, flat) takes "Guard damage" (12) through `GameUnitComponent::damage`. It never misses (no projectile). **ON A WALKWAY DECK, a ward's top or the landed ship's rail** (`guardDeckAt`) nothing blocks it: the guard shoots over the walls. **On any other road cell it needs a CLEAR LINE** from its eyes (1.3 m) to the unit: `nearestEnemyUnit`'s `sightFrom` casts one ray at static bodies only (layers Default, Gate, Terrain, Vegetation: buildings, walls, gates, the ground, trees and rocks; units and the Walkway scaffold do not block), for the 6 nearest units at most, and takes the nearest one in sight. The shot is a `GuardShot` record; the match draws it as a hit line (`NpcSystem::addBeam`, `MeleeHit`, so GLt sends it to clients). |
| A new post | With nothing to shoot for 4 s a guard runs `guardPost` again and walks to a better cell (the threat moved). |
| Stand down | The alarm is over, the Armory is gone or rubble, or production took the worker (`dismissed`): the guard gets its `carrier.pre` look back and takes carrier work from where it stands, or goes home. |
| Badge | "No road" on an Armory (or a Mage tower) that touches no road. |
| Ring | The POST RANGE as a ring (`drawDebug`): on an Armory, a Mage tower or the Beacon while it is selected, and on every built one (and the ghost, `MatchInput`) while an Armory or a Mage tower is on the ghost. |

**THE MAGE TOWER** (`EStructureType::MageTower`, `magetower.pre`, 2x2, the Unit page) is an armory of STAFFS:
`armsGuards` includes it, so the alarm, the call, the posts, the stand-down and the road rules above are the same.
Its walkers are MAGES (`Walker::mage`, set when the job is a Mage tower; the look is `mage.pre`), "Mages per tower"
(3) at most. A mage does not shoot: every 0.25 s it picks the nearest enemy unit within "Mage range (m)" (16) by the
guard's sight rule (anything from a deck, a clear line elsewhere) and turns its CONE to it (`Walker::yaw`,
`Walker::lit`). `publishSearchlights` writes each lit mage's cone into `GameUnitParams::lightCones` - a small
Searchlight: "Mage cone half angle (deg)" (14), "Mage damage/s" (1.5) to each enemy unit in it, which is solid there -
and a SpotLight at "Mage light brightness (x)" (0.3) of the Searchlight's. Mages exist on the authority only, so a
client sees neither them nor their light. They need no light store.

### The wards

**Three ward types** (`EStructureType::WardResist` / `WardAbsorb` / `WardRetaliate`, `isWardType`, `wardKindOf` 0 / 1 / 2;
the Structure page; "Game/Ward" tweaks; `Wards.cpp`): a 1x1 WARD TOWER - a full 2 x 2 x 2 m stone block, its flat top at
a Walkway deck's height, with a light crystal floating over it (`wardResist.pre`, `wardAbsorb.pre`, `wardRetaliate.pre`:
one tower, three colours; the crystal at 2.65 m over the origin = `beamHeightOf`, its lowest point 3.95 m over the
ground, so a player on the top walks under it). **A ward between walkways carries the walk on:** the deck solve takes a
built ward's top as a hard edge (`solveWalkwayDecks`, see The Walkway), and the magic stays off the walk - a glowing band
round the block under the decks' planks (5 mm proud of the faces), a glowing rune flush in the top (2.5 mm over it), the
floating crystal. The collider is the block alone. **A built ward's top is ROAD, like a Walkway deck** (`roadAt`, at
`wardTopY`: a deck origin's height): the carriers walk over it, a building next to it is on the road, and the guards
and mages take a post on it (a high place: they shoot over the walls). It is not a cable: the road's runs do not pass it. Tried a 3.5 m pylon, dropped because between walkways it left a gap
in the walk. 200 health. Each holds a LIGHT STORE ("Light store", 200) that pays for its work. It takes light like any
consumer (a hub link), at **"Refill (light/s)" (2) at most** - in `tickLight` and, with the beams off, as the metered intake
of its transport slot.

**The latch** (`updateWardLatches`, every instance, every tick, from the mirrored store on a client): a ward whose store
drops under 1 light is EMPTY and switches off (`Ref::wardDrained`: no shares, no regrowth, no pieces, the "Charging"
badge) until its store is back at **"Restart at (share of store)" (0.25)** of its capacity. So a ward that is drained
faster than it refills does not flicker at the refill limit, and a new ward charges up first. `wardActive` = built, not
latched, store over 0.01 (the store that a payment emptied this tick protects no more).

| Step | Rule |
|---|---|
| The links | `rebuildWardLinks` (every instance, every 0.5 s and at once when the roster size changes): each BUILT ward, in roster order, runs ONE breadth-first search from the built wall and gate segments (`isLineType`, its team) on the 4 sides of its cell, through the 8 cells around each segment (a line dragged at a slant joins at the corners). It visits **"Walls per ward" (16) segments** at most and covers the ones that no earlier ward OF ITS KIND covers (`Ref::wardOf[kind]`); a covered segment is still walked through, so a ward behind another one reaches the walls past it. `Ref::wardWalls` counts what it covers; 0 = the "No wall" badge. Rubble is a blueprint, so it breaks a line. |
| The flag | A covered segment gets `GameStructureComponent::warded`, and `damage()` reads the union's `wall` variant (`WallData`). A segment that comes under a ward or leaves it starts clean (no shield, nothing owed). |
| The hit | `damage()` (atomic, from the workers): **Retaliate** - its share ("Retaliate share" 0.5) of the WHOLE hit goes back (the return value; the unit's bite banks it on the biter through its shadow too: `GameUnitComponent`'s bite); **Resist** - its share ("Resist share" 0.35) is taken off; **Absorb** - the segment's shield takes what is left first; the rest reaches health. Every damage path goes through it (bites, shells, a field's drain, the blast); only a bite has a biter to send damage back to. |
| The pay | `tickWards` (authority, every tick, after `tickLight`): each covered segment hands over what it resisted and sent back since the last tick (atomic `resisted` / `retaliated`), and the ward pays "Resist light per damage" (0.1) / "Retaliate light per damage" (0.05) - an overdraft of one tick stops at an empty store. The Absorb shield grows back at "Absorb regen (health/s)" (3) per segment up to "Absorb shield (health)" (200), "Absorb light per health" (0.1), as far as the store pays - but only **"Absorb regen delay (s)" (4) after the segment's last hit** (`damage()` sets `WallData::struck`; `tickWards` takes it and restarts `regenWait`), so a shield under attack does not grow back. Then the shares for the next hits: 0 for a kind whose ward is missing, empty or latched off. |
| The look | `updateWardLooks` (every instance, every 0.25 s; `Models/wards.obj` through `wards.oc`: LIT, like the Walkway's pieces - each piece prefab's `Color` is its tint, and the sun, the lights and the shadows fall on it): on each OPEN side of a covered segment (a cell with nothing, or only a flat road or cable piece) - **Resist**: a full-size pane (`wardResistPane.pre`), plus a post at each outer corner where two panes meet (`wardResistCorner.pre`), so a wall line and its corners have one barrier with no seams; **Absorb**: a smaller pane in front of it (`wardAbsorbPane.pre`), on the authority only while the segment's shield holds 1 or more (a client does not know the shield: it shows it while the ward has light); **Retaliate**: five cones out of the wall (`wardSpikes.pre`). Each piece is modelled for the +X side and turned to its side. The pieces are root entities owned by the segment's roster entry (`Ref::wardPieces`), spawned again only when what they show changes (`Ref::wardLook`). The renderer has no see-through meshes, so the panes are solid. |

**Not saved:** the links (derived) and the Absorb shields (they grow back after a load). The ward's store is saved like any
store.

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
stores — "Emitter buffer" 50, "Bastion buffer" 150 (the emitter restart charge
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
the same rings) — plus the node rings, the built yards' harvest radius, the constructor reach rings, the Healer's hut's reach (only the selected hut, or every one while a hut is on the ghost) and a red ring on every
unpowered consumer.

Segments are full structures: a blueprint phase (**NON-conductive until built**), health (40; a crossing 60 —
**every type has its own health**: `GameStructureSettings::health`, indexed by `EStructureType`, the "Game/Health"
tweaks, through `StructureSystem::healthMaxOf`: the Beacon 1000, a Wall 1000, a Gate 800, a Great lantern / Barracks 400 (a unit's bite on a structure is its `StructureDamage` - by default its `AttackDamage`; the siege types author more: Elite 16, Brute 60, Giant 240, Titan 900 - x "Bite damage on structures mult" 0.5, `GameUnitParams::structureBiteMult`),
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
> `fundNearbyBlueprint` skip it, so no worker walks to it and the stone goes to the next blueprint. **A PLAYER is
> exempt** (`byPlayer`, the player materials loop): it builds rubble up with enemies near; only a body on the pile
> stops it. **The enemy test is CACHED** (`Ref::rubbleUnsafe`): `refreshRubbleSafety` (authority, every 0.25 s, before
> the Builder's huts) runs ONE sphere query per pile, and a fresh pile counts as unsafe until it looks (`setRubble`); the
> body on the pile (`actorInFootprint`) is checked live. `fundNearbyBlueprint` sorts the candidates in reach by distance
> first and tests the rubble rule only down that list (a query per pile per hut per frame made a late game with much
> rubble and a large wave cost tens of ms). It
> rides GSt's status byte (bit 4) and the save key `Rubble`. **A ROAD PIECE is rubble too** (the cable
> types: Road, Bridge, Walkway): broken paving (`rubbleRoad.pre`, `Models/rubbleRoad.obj`) in place of its meshes
> (`updateArms` keeps the arm pieces off), no road for the carriers while it is a blueprint, and built up again like any
> other. It is outside GSt, so its flag rides GCb's top bit, and the built re-send of GPl clears it. A blueprint that
> never stood and a crossing are removed as before; the Base never dies.

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
  within "Constructor range" (27 m) at "Constructor build rate" (4/s). A REPAIR (a built structure) is
  also limited to "Constructor repair health/s" (20, x staffing; `investMaterials`' `repairHealCap`): by the stone price
  alone a cheap structure with much health (a Wall: 5 stone, 1000 health) would repair at 800 health/s. A blueprint has
  no such limit, and the workers' repair deliveries have none. With nothing to build or repair it idles.
  > **The focus crystal aims.** The Builder's hut is a 1x1 pylon tower (constructor.pre, 3.5 m); its floating `Head`
  > on top (a violet focus crystal along its +X, a small stone behind it) turns toward its work at "Constructor turn
  > speed (deg/s)" (90), and **the hut puts stone in only while the crystal points at it** (within ~12 degrees,
  > `turnBuilderHead`): a new job waits for the turn. `tickConstructors` picks the work with `findBuildTarget` (the pick
  > `fundNearbyBlueprint` makes) and keeps it on the Ref (`buildTarget`, `headYaw`, `buildWorking`: stone went in this
  > tick). A MIRROR derives the same from the mirrored roster (`mirrorConstructors`: the hut built, powered, with stone;
  > `refreshRubbleSafety` runs there too), so nothing of it is on the wire - for a moment it can pick another job than
  > the authority. While it works, `drawBuilderStreams` (every instance) draws a STONE STREAM from the crystal's point to
  > the work: `Effects/builder_stones.pfx` ("Constructor stones/s", 14; 0 = none) and `builder_sparks.pfx` (1.5 x as many
  > violet sparks), through `ParticleSystem::emitStream`, within 300 m of the camera. None while it repairs itself.

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
| 1×1 (cables included; the Bridge and the Walkway are cables) | Emitter, Wall, Gate, Lens, Constructor, the three wards |
| 2×2 | FuelTank, Generator, Battery, Bastion, Turret, Searchlight, Solar, Lumberyard, Quarry, Fabricator, MineralSilo, House, MedicStation, Armory, MageTower |
| 3×3 | Barracks, Base |
| 3×1 / 1×3 | **the three crossings** — the ONE non-square footprint, by its facing |

**ON THE SHIP'S DECK** the same footprints snap to the deck's own tiles in ship space, and the free deck tiles of a
LANDED ship are road (`roadAt`): see Building on the ship in [Ship.md](Ship.md).

> A crossing's facing is quantized to ±X/±Z at `placeStructure`, and carried on GPl
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
* **Bridge** (`bridge.pre`, 1×1; no slot: the Road's stroke places it over water): **A ROAD SEGMENT ON A DECK** - `isCableType`, the conveyor
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
* **Gate** (`gate.pre`, 1×1, a drag line; the Structure page): a wall segment whose top is a wall's height (3 m) over the
  surface, with a foot of two pieces down to 12 m under it; the collider covers all of it. `Layer Gate`,
  `CollidesWith Default Projectile`: **the PLAYERS (layer Player) walk through it, every unit does not** (the layers
  cannot tell teams: own units are stopped too, and an enemy PLAYER passes). Nav takes it as a wall (`isBreachable`).
* Water deeper than the pier / the foot leaves a gap under it. The aim ray hits the BED (the terrain collider), so
  over deep water the ghost is a little past the cursor.

**THE WALKWAY (`walkway.pre`, 1×1; the Structure page): A ROAD SEGMENT ON A SCAFFOLD**, to stand behind a wall and see
over it. Like the Bridge it is `isCableType` of the conveyor medium: in the road's runs, walked by the carriers,
painted by a stroke, cable health, no label, outside GSt. Its deck top is `StructureSystem::WalkwayDeckHeight` (2 m)
over the ground; the origin is 0.1 over the deck (`spawnHeightOf` = deck + 0.1). **It stands on WATER too**
(`standsOnWater`, like the Bridge and the Gate: `groundFit` takes the water surface as its ground, so the deck is 2 m
over the water), on a pier of three render-only pieces down to 11 m under the sills (in the ground on land).

* **Not walk-through** (`isWalkThrough` leaves it out): nothing fits under the deck, so it is a Nav obstacle - a
  BREACHABLE one (`isBreachable`) for the enemy, so a line of it does not close a base off - and a body in the cell
  refuses the placement. **For its OWN team it is a wall** (Nav `wallFor`, `submitNavFeed`): Nav is flat and has no way
  up the stairs, so the own army paths around it (it also is Blocked in the raster: steering and lanes go around).
  A barracks unit's spawn point (`freeSpawnPointAround`) tries 4 rings out, 2 m apart, and only then a walkway DECK
  cell next to the barracks, where it spawns on top.
* **Two entities.** The structure (`walkway.pre`) is the sills on the ground plus a projectile-only collider
  (`Layer Cable`) from the ground to the deck. What a body stands on is `Ref::walkPiece`, a root entity of its own that
  `updateWalkway` (from `updateArms`, so on every `rebuildNetworks`, on every instance) spawns for a BUILT piece: a
  blueprint has none.
* **Deck or stairs** (`Ref::walkDir`): `walkwayDeck.pre` by default. `walkwayStairs.pre` when a PLAIN road piece (a
  Road or a Bridge, blueprint included) is a neighbour AND the stairs continue the walkway's line (a Walkway on the
  opposite side) or the piece has no Walkway neighbour. So a walkway's END at a road goes down to the ground, and a
  walkway that runs ALONG a road keeps its deck. The stairs are one box from the deck's height at the far cell edge to
  the ground at the road's edge (45°), spawned at the middle of the slope with a yaw toward the road and a pitch - a
  structure's own collider cannot be pitched, which is why the piece is a second entity.
* **THE DECK TILTS to meet its neighbours** (`solveWalkwayDecks`, then `updateWalkway`): on uneven ground every
  walkway's deck is 2 m over its own cell's centre, so a run made steps a body caught on. Every deck is a PLANE (a
  centre height and a slope along X and Z), solved for all decks at once by 8 Jacobi relaxation steps (the same result
  on every instance, whatever the roster order). Each step, an edge shared with a neighbour DECK aims at the middle of
  the two planes' heights there, an edge at a neighbour STAIRS' top or a built WARD's top at that height (a HARD edge: they do not
  move), every other edge at the deck's own height; a neighbour more than 1.2 m higher or lower is a real step and is
  not met. Each deck then fits its plane: the centre is the mean of its axes' middles (an axis with a hard edge weighs
  4x, one with no neighbour nothing), and an axis' slope comes from its hard edge when it has one (that edge is then
  exact), else from its two edges. Straight runs and runs onto stairs are seamless; a corner or a T keeps a small step
  (a plane cannot meet four edges), shared between the decks. `rebuildNetworks` picks every walkway's deck or stairs
  first (`walkwayModeOf` -> `Ref::walkWant`), solves, then poses the pieces; the tilt is `Ref::walkLift` (the centre
  over the walkway's origin) and `walkSlope` (rise per m), and a new tilt spawns the pieces again (a static body does
  not follow a moved entity) - only when a neighbour comes, goes or is built. The readers take it: `roadAt` (the
  carriers: the centre), `cannonStandY` (a cannon stands level on the centre), `walkwayDeckTop` (`walkwayDeckY`: the
  height at the point - the barracks' spawn on a deck).
* **Only the deck tilts, the LEGS stay upright:** `walkwayDeck.pre` is the planks, the ledgers and the rails (the
  rails are short bars between the legs, so they tilt with it); `walkwayLegs.pre` (`Ref::walkLegs`, a root entity of
  its own, untilted on the walkway's origin) holds four `WalkwayLeg`s (2.6 m long, the top at the entity's y), each
  moved up or down at spawn to the tilted ledger over it - the long foot goes into the ground.
* **Both colliders are SOLID down to the ground**, so no body gets under a walkway (one that walked into the scaffold
  got stuck there): the deck is ONE box from its top to 0.5 m under the ground (the 0.5 m covers a sloped cell), the
  stairs a board 1.4 m thick under the walk surface (at 45° that fills the ~2 m wedge under it; its ends go into the
  ground and into the deck's box).
* **Carriers:** `roadAt` gives a stairs cell the height of the middle of the slope, so a walker goes up between the
  road cell and the deck cell (its straight path is up to 0.5 m off the board at the two cell edges).
* The model is `Models/walkway.obj` (hand-written boxes: `WalkwayFoot`, `WalkwayDeck`, `WalkwayLeg`, `WalkwayStairs`).

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

**THE SHAPES.** Fourteen buildings are placeholder models of the setting (stone, timber, bronze, light crystals):
the Base, Emitter, Bastion, Turret, Wall, Solar, Battery, Generator, Barracks, House, Healer's hut, Cannon, Ammo works and
Ammo store. The ROOT's Render is
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
building now), and small ones sit on the Turret, Solar and Barracks (a green one floats over the Healer's hut). The flat maps' two resource
nodes are models too: a rock outcrop with ore (`mineralNode.pre`) and a log stack (`fuelNode.pre`). The Lumberyard
(`lumberyard.pre`, brown) and the Quarry (`quarry.pre`, grey) are placeholder boxes.
