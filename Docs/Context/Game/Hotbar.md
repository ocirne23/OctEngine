# Hotbar and interaction modes

> Part of the Game documentation ([`Docs/Context/Game/CONTEXT.md`](CONTEXT.md)) — read that first.

## Hotbar and interaction modes

### The RTS grid hotbar

12 slots on the GRID HOTKEYS **QWER / ASDF / ZXCV**, row-major, always visible in game mode. **Every
slot is also CLICKABLE**: `slotAtScreenPos` → `activateSlot`, the ONE entry point for key and click.
Clicks over the hotbar never reach the world.

Every caption is a 3–5 char SHORTHAND from `c_structureShortNames`, indexed by `EStructureType` —
**the SAME table the world tag over a building uses, so a slot and the thing it builds read
identically.** A build slot's count is `affordableCount`.

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
| Q / W | **CMBT / PROD** | the category's items, in order (slots 0–8) |
| A / S / D | **CBL-P / CBL-F / CBL-M** — the cables arm straight from the root (a hidden `c_cableCategory`, still drawn as the root page: `isRootPage`) | items 4–6 |
| X (slot 9) | **DEL** — toggles Delete mode: one demolish, then straight back to Select | **DEL** (category pages cap at 9 items so X stays Delete) |
| C (slot 10) | **CNCL** — leaves Delete mode / disarms a cable, back to Select | **CNCL** — straight back to Select |
| V (slot 11) | — | — |

**Esc / Tab = `cancelOneLevel`**: a two-click step drops → the armed item disarms → the page or
Delete mode returns to Select, **one per press**. **C = straight back to Select, whatever was
armed.** F9 / F10 save and load (see Save / load ([CONTEXT.md](CONTEXT.md))).

`escWouldCancel()` is true while Esc still has an in-game meaning, and **the escape MENU only opens
when it is false — so the cancel chain keeps first claim.**

### Q — Combat

**Emitter · Bastion · Lance · Wall · Turret · Barracks · House**

* **Bastion** — a big, hungry anchor bubble. **Lance** — a focused cone; **TWO-CLICK placement**: the
  first click anchors, the second aims the facing.
* **Wall** — a DRAG line of square segments every 2 m (`c_wallSegmentSpacing`): the press anchors, the
  release places. Per-segment cost, `c_wallMaxSegments` 16.
  > **A BUILT wall is fed to Nav as a BREACHABLE obstacle** at "Wall breach cost" (10) — one 2 m cell
  > costs (1+cost)× the walking distance, so enemies route through a wall rather than take a longer
  > detour, walk into it and bite it down. **Blueprint walls and every other building stay
  > impassable.**
* **Turret** — HITSCAN lightning. The component picks the nearest enemy unit in "Turret range" (18 m)
  and lands "Turret damage" (25) on it; **it never misses.** The strike is a BUNDLE of jagged debug
  lines fading over "Turret beam lifetime" 0.5 s (`NpcSystem::addBeam` / `drawBeams`), broadcast to
  clients as GLt, plus a muzzle FLASH.
  > **BEAMS AND FLASHES** (`NpcSystem`): a beam has a kind — `Turret` (the lightning) or `MeleeHit`
  > (a unit's swing, see Targets ([Units.md](Units.md))). **The muzzle FLASH is not a separate record:** `drawBeams`
  > pushes one point light at every live Turret beam's `from` (`addLightInfo` with a `PointLight`, a per-frame
  > record) with the intensity fading linearly over the beam's lifetime, scaled by "Muzzle flash
  > intensity" — so it lives and dies with the bolt. GLt carries the kind byte; melee hit beams
  > are relayed at most `c_maxHitBroadcast` 64 per service, turret strikes always.
  > **There is NO impact flash: the VICTIM lights itself** — `GameUnitComponent::tickHurtLight`
  > (every role, before the client gate) compares health against the last tick's and pushes a red
  > point light over the collider's top while it drops, decaying over "Hurt light decay (s)" after
  > the last drop. So a melee hit, a turret strike, a shell AND force-field exposure all light the
  > unit, and clients see it off the replicated health. "Hurt light intensity" scales it.
  > **Flash counts never scale with the crowd:** "Light area (m)" buckets the world and "Hurt
  > flashes/s per area" caps the flashes in each bucket (see the Entity CONTEXT's
  > GameUnitComponent notes). The shield/emitter GLOW is the Force system's bubble light
  > ("Force/Glow/Bubble light …" tweaks): one light per bubble, sized by the bubble radius.
  > **THE ENERGY STORE IS THE RELOAD BAR** (the barracks rule; there is no fire timer):
  > `stampTuning` sets its energy capacity to "Turret shot energy" (**2 — keep it a WHOLE number:
  > the transport delivers whole cells and a consumer asks for `floor(capacity − store)`, so 1.5
  > stalls one cell short**) and its transport slot meters its intake to shotEnergy / "Turret fire
  > interval" (1.2 s), so a fed turret fires at exactly the authored cadence, a starved one fires
  > slower, and a full turret with no target holds its charge and fires the instant one appears. Its
  > bar is progress-green and always shown.
* **Barracks** — ONE type, 3×3, cable-fed. The Brute/Runner/Spitter barracks are RETIRED enum
  slots; `loadFrom` maps them to a Barracks with that unit type. See below.
* **House** — 2×2, no grid role. Every built house links to the nearest built own-team barracks within
  "House link radius" (25 m), **one barracks per house**, re-derived every `refresh()` on every
  instance by `linkHouses`. Drawn as a line to its barracks plus the radius ring while selected.

#### Barracks detail

Selecting an OWN barracks shows a UNIT-TYPE PICKER popup above it (`HudPopup` in `Core.GameHud`, drawn
by GameHudOverlay; clicks resolve in `updateGameInput` through `popupButtonAtScreenPos` **before** the
hotbar). Options (`c_barracksMenu`): **Grunt / Warrior / Brute / Runner / Swarm**.

**The pick is an ORDER, like the route**: `queueUnitTypeRequest` → validated in `tickAuthority` →
`onUnitTypeChanged` → GBu broadcast plus join replay. Clients request through GqU; saved as
`UnitType`.

Each spawn pays the type's "\<Type\> spawn energy" from the energy store, whose capacity IS that
cost and which fills at the capped "Barracks energy intake/s" (a TOTAL over all its power links) —
**full store = a unit, so the bar over the barracks is the build progress and the price IS the build
time** (cost / intake). The spawn check carries a 0.01 epsilon so a fill that lands a rounding step
short of the cap still counts.

It holds the type's "\<Type\> population" against the **POPULATION CAP = "Barracks population" (20) +
"House population" (10) per linked house**. A spawn only happens while `population + cost <= cap`
(`BarracksData`, tallied by the spawn and death edges — the unit's `popCost` rides its DeathRecord).
**The population rides GSt in the barracks' otherwise-unused output byte.** Spawns land on a free
cell on a ring around the building, on the side facing the route's first waypoint.

#### Barracks routes

In Select mode (or Build with nothing armed) with an own-team barracks selected, **RIGHT-click on
ground sets a waypoint**; SHIFT+RMB appends, max `MaxRouteWaypoints` (6) — **bounded by the 64 B GqW
request.**

Spawned units march the waypoints (advancing on touching each "Waypoint radius" circle, **no combat
mid-march** — except an enemy unit within "Route engage radius" (5 m), which diverts the walk target
and resumes after) before their AI takes over. The barracks route is copied onto the unit at spawn AND
**re-pushed to its live units when the route changes** — the march index is kept and clamped, so
appended routes continue. Every route change also seeds a Nav lane, leg by leg (`seedRouteLane`).

Server-validated (own-team barracks only), mirrored through GRt plus the join replay, drawn as a green
line chain with circles for the own team.

### W — Production

**Generator · Solar · Extractor · Fabricator · Constructor · Battery · Mineral silo · Fuel tank ·
Medic station**

Solar is a free trickle with no fuel (a flat `CubeQuarter` slab); the fabricator turns energy + fuel
into minerals; the mineral silo is the team's spendable bank.

* **Medic station** — 2×2, a plain powered consumer ("Medic energy/s" 1.5 from its internal buffer,
  unpowered = red ring). While built AND powered it heals every body inside "Medic heal radius"
  (12 m, green ring) at "Medic heal/s" (4) — **HEALTH and the SHIELD BATTERY both**, the same rate
  each. **UNITS: the station's own component update** (`EMachineKind::Medic`, the turret pattern —
  one spatial query of the radius per powered station inside the parallel pass, banking
  `GameUnitComponent::heal` into the unit's heal inbox; the unit's own tick applies it to health and
  battery — after the death check, so a heal never revives — and clears its `collapsed` latch once
  the battery holds charge). Stations STACK on units (two radii = two heals). **The own player**:
  `GameMatch::tickMedicHealing` on every instance (`GamePlayer::heal` + `charge`; health/energy are
  owner-computed, `powered` arrives through GSt), once per tick however many overlap. The reach/rate
  tweaks bind `GameStructureParams::medicRange/medicHealRate`. The ghost shows the reach.

### A / S / D — Cables (root page)

**Power cable · Pipeline · Conveyor** (slots `CBL-P/F/M`). **The crossings are NOT on the hotbar** —
only the paint stroke places them (auto-crossing below).

Cable segments place by **PAINTING only** (`updateCablePlacement`): an LMB press starts a stroke at
its cell, holding and dragging keeps placing the cells the cursor crosses (L-filled between cursor
samples by `placeCableLine` — dominant leg first, **occupied cells skip so a stroke across an
existing run fills gaps**, so the run never breaks), and the release ends the stroke.

**Auto-crossing.** The stroke HOLDS its newest cell back one step (`m_cablePending`). When the cell
after it is a bridgeable SOLE occupant of ANOTHER medium (`bridgeableAt` — a plain segment or another
crossing's end cell, with nothing bridging it), the stroke places **the stroke's OWN medium's crossing
over that cell with its long axis along the stroke** (`crossingForMedium`): the held cell and the cell
beyond become the crossing's two end cells and are never painted, so the painted run continues
through the crossing's outward ends. **An end cell that already holds this stroke's own medium is no
obstacle: `planCrossing` replaces it** (see the cell hash ([Economy.md](Economy.md))), so re-crossing
a line where the run already stands works. A crossing the cells still refuse (an end on another
medium or a building, an actor on it, an L-turn at the foreign cell) falls back to the plain skip.
Same-medium cells still just skip — the stroke merges into that run. The release AND the RMB cancel
both land the held cell (it was already shown painted); switching items or disarming drops it.

One placement burst caps at `c_cableMaxSegments` 32 cells.

### The Bases

They exist from game start only (`spawnBase`; `isPlaceableType` keeps Base and the retired types
off placement), one per team.

* Respawn anchor.
* A passive trickle of Minerals at "Base income mult" (0.25) of an extractor.
* MINERAL + ENERGY storage, **no fuel**: spawned with a full energy store and "Start minerals",
  self-generating "Base energy gen/s"; power cables attach.
* **Its SHIELD runs on EMITTER RULES, minus the bill** — `hasShieldEmitter` includes Base in
  the production tick, `strainable`, mirror and save: unit siege drain, latching dark when starved,
  output and reach from the "Base shield output" / "Base shield reach" tweaks (which override
  `base.pre`'s authored values) — but **no per-second draw and no pressure surcharge** ("Base shield
  energy/s" is unused): the Base is a STORAGE building on the energy grid (a transport storage
  port). Only enemies leaning on it drain it.
* DAMAGEABLE but **the death sweep never destroys it**: at 0 hp it stands dead until repaired.
  Constructors heal it at its `costs` entry (100) — **never placed, so that entry only prices
  repairs**; `investMaterials` admits Base alongside placeables. Delete mode refuses it.

> **A destroyed Base would soft-lock the respawn.**

### Structure health and labels

Every structure taps the pressure bake at its position (`sampleBakedField`, no GPU query slot);
**territory owned by ANY other team's bubble drains health** ("Damage/s in enemy field").

**THE EMITTER GLOW is the Force system's bubble light** (no `Component Light` on the emitter
prefabs): one point light per bubble at its centre, sized by its iso radius, so it grows and fades
with the powered field and goes dark with it — see [`Docs/Context/Force/CONTEXT.md`](../Force/CONTEXT.md).

Emitters shrink out over "Emitter shrink time" when starved and **latch off until "Emitter restart
charge"**, pay a pressure surcharge ("Emitter energy/s @ pressure 1") plus a per-unit siege drain
(`GameStructureComponent::addLoad`: a flat rate onto the NEAREST strainable enemy emitter within
"Game/Enemies/Emitter drain range (m)" — 20 m, the Bastion's visible bubble radius at reach 45 /
output 2.6, planar distance to the STRUCTURE, shield state irrelevant; spitter shots deposit too,
within their own `EmitterDrainRadius`).

**World labels are CULLED before they are built** ("HUD/Label max distance", 120 m, not Synced,
**measured from the PLAYER entity** — the camera only when there is none): units come from a frustum
query bounded by that distance plus the camera-to-player distance and then test the exact player
distance, structures test it against the anchor (the selected one is exempt), and both drop anything
`worldToScreen` puts outside the viewport plus the overlay's 100 px margin — `worldToScreen` itself
only rejects what is BEHIND the camera.

**World labels:** a health bar over every damageable structure, plus a second bar for storage (energy
yellow, fuel orange) and a third on the Base. **FULL health and shield bars stay hidden** — only
damage, blueprint progress, selection, or a prefab's `AlwaysDisplayHealth true` shows one. Undamaged
non-player units skip their label entirely; `player.pre` opts in so player tags persist. A SELECTED
cable segment or crossing adds its transport readout — "Energy 3 / 4 cells, 8.0 / 10.0 per s" plus
"Run 41 / 96 cells over 24 segments" (`StructureSystem::cableInfo`: the segment's fill, the cells
leaving it as a ~2 s average against its out-rate — also its second bar, hued by medium — and its
run's total; clients read both from the mirror). A segment at or above 90 % of its rate draws a
pulsing RED ring: the bottleneck marker, and the only cable ring.

### Unit selection

Select mode (and Build with nothing armed), **authority only** — units simulate on the server.

**LMB DRAG (> 8 px)** draws a box on the ground and on release selects every visible own-team unit
whose screen position is inside (`NpcSystem::queryVisibleUnits` — a camera FRUSTUM query — plus
`worldToScreen`). SHIFT adds; a plain click clears. `m_selectedUnits` are owning EntityPtrs pruned on
death, drawn with a green ring.

**EVERY RMB MOVE ORDER** — ground, held re-aim, or building face — **also goes to the selected
units**: `orderMove` = a LOCKED target with `moveOrder` set (dropping the route), auto-cleared within
`waypointRadius` so the AI resumes. A DSL `setTarget` lock never clears. **CTRL + RMB = units ONLY**
— the player stays put (`m_rmbUnitsOnly`, latched on the press for the whole hold). A FRESH order
seeds one Nav lane from the centroid of the selection's largest cluster ("Game/Nav/Group cluster
radius" 12 m).

A FRESH order can make the unit IGNORE the lane term for "Order flow blind" seconds so an old trail
cannot pull it back — **0 by default, since a fresh order SEEDS its own lane.**

### Interaction modes (`EPlayerMode`)

**Neutral / default = SELECT.** Click to inspect (highlight ring plus overhead info); RIGHT-click
GROUND with an own barracks selected = a waypoint; RIGHT-click a building = a MOVE ORDER to it.

**Q/W on the root page = Build** with that category, **A/S/D = Build with a cable armed** — **the
hotbar page doubles as the mode indicator.**

**Build mode ALSO click-selects** (`updateSelectionClick`, shared with Select mode): with nothing armed
every LMB inspects (and box-selects / sets routes as in Select), and **with a ghost armed, a click the
placement refuses — occupied cells, i.e. on a building — selects it instead of doing nothing**, while
any click that CAN place still places.

#### RMB always moves the player

Cancelling rides along on the SAME press:

1. **Whatever step is pending is cancelled, one level at a time.** A half-finished two-click flow
   (Lance aim, Wall line, cable paint stroke) drops first; the next RMB disarms the item.
2. **With an own barracks selected and the cursor on GROUND**, the press is instead a route waypoint
   (SHIFT appends). **This is the ONLY consumer that sets `m_rmbConsumed`**, because a rally point is a
   positive order, not a cancel, and pairing it with a move would drag the player to every waypoint.
3. **Everything else becomes a MOVE ORDER** (green destination ring). On GROUND the capsule walks to
   the clicked point and **HOLDING RMB keeps re-aiming at the cursor**. On a BUILDING the destination
   is the CLICKED ground point pushed out to the nearest face of the inflated footprint (half
   footprint + 1.2 m) when it fell inside — **so you walk to the side you clicked instead of grinding
   into the wall.** One-shot; it does not drag. Walk-through pieces (cables, crossings, solar) count
   as ground.

**Cancels deliberately do NOT consume** — a cancel that also ate the movement reads as a dropped
input.

A respawn drops a standing order. There is no WASD movement (the letter keys are grid hotkeys), and
**because RMB-hold steers, the camera yaw drag is on MIDDLE-mouse**; LEFT/RIGHT arrows also yaw, and
the wheel zooms.
