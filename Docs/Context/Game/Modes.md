# Game modes: co-op PvE and PvP

> Part of the Game documentation ([`Docs/Context/Game/CONTEXT.md`](CONTEXT.md)) — read that first.

## Co-op PvE (`--coop`)

**Co-op is the SANDBOX WORLD** (`GameMatch::m_sandboxWorld` = `m_coop`; `sandboxWorld()`): the flat maps and the
rock grid are PvP's. "Game/Coop" "Spawn enemies" (on) gates the waves (the wave HUD counters go with them); off = a
peaceful building game.

**The setting is [`Docs/GDD.md`](../../GDD.md)**: light is safety, demons come at night - see "The light rules" below.

### The sandbox world

The procedural terrain, trees, rocks and ocean of the sandbox, with the third-person player. `Session::startWorldAndGame`
switches `Terrain/Enabled` and `Ocean/Enabled` on through overrides (exit-to-menu's `unseedWorld` switches them off) and
hands the match its **anchor** (`setWorldAnchor(pos, dir)`): by default the sandbox camera's start and direction
(`--camera` / `Session::COOP_START_POS` / `COOP_START_DIR`: the co-op game's own start, apart from the sandbox camera's) on the sandbox's own seed ("Terrain/Seed", which the lobby's seed
field shows until a preview runs) — the same world and spot as the sandbox; or (0, 0) for a world the local lobby
seeded (its pick IS the origin). **The whole world streams**: the sandbox's "Terrain/Range (chunks)" ring, and a
lobby-seeded co-op start gives the ring back its size, lifts the generated bounds and keeps the terrain models
loaded (`Session::serviceLobby`), so chunks past the seeded area generate live. No ground box, no rock grid, no
barrier, no placement bounds; the shadow preset and the cloud suppression of the top-down camera are NOT applied.

**The Base site** (`tickBaseSite`, authority): the camera waits at the anchor while the terrain streams in (the
collider ring follows the camera). Once the terrain collider answers under the anchor, it waits 2 s more (the tree and
rock world colliders follow the terrain). The local player spawns ON the anchor (the ground under it + 1.5 m), the
camera looking along the anchor direction. **THE BASE IS THE SHIP**: the ship starts FLYING 50 m over the anchor with the
Base (the Beacon) on its deck and the player at its helm - the start and the landing rules are in [Ship.md](Ship.md)
(The start, Landing). With "Game/Ship" "Enabled" off the Base stands on the land: the nearest point on 4 m rings out to 56 m
around a point 14 m ahead where it passes the ground test (see Grid placement in [Economy.md](Economy.md)) and its
footprint stays 2 m off the player's spot. No site → the Base goes on that point ahead anyway (warning); no ground at
all after 60 s → the anchor's own height.

**No resource nodes:** wood and stone come from the world itself. A Lumberyard works the trees around it, a Quarry
the rocks (The harvest in [Economy.md](Economy.md)).

**The player** (`GamePlayer::setDirectControl`): `playerCoop.pre` — `player.pre` with `ThirdPersonAnimator` and no
script — driven by camera-relative WASD at the "Network/Player" speeds, Space jumps, LShift sprints (the shield battery
rule stays), and `Game:PlayerLocomotion` (the testbed key C capsule's camera-locked strafe, shared with App) turns the
body. Kill floor 1000 m down instead of y −20. The camera is `GameCamera::applyThirdPerson`: mouse look in RELATIVE
mouse mode (the App holds it while `wantsMouseCapture()`, the viewport is focused and no menu is open), the wheel
zooms ("Game/Camera" "Third person ..."), a ray pulls the eye in front of the TERRAIN only (a building, a tree, a
rock or a unit between the two does not move the camera: the player may be hidden), a ray down keeps it
"Third person min height" over the terrain under it (0 = off), and the clip planes are the fly
camera's ("Editor/Camera Near / Far") so the view reaches the horizon. **Aim = the crosshair** (GameHud
`setCrosshair`): `m_mousePos` is the viewport center, so placement, hover and picking all ray through it, and the aim
ray hits the TERRAIN layer only. RMB orders only the selected units (the player walks with WASD).
**THE STAFF BEAM: HOLD LMB** (PLAY: the build UI closed, Select mode; `m_beamHeld` from `updateGameInput`,
`GameMatch::tickStaffBeam` on the authority after the structures' tick). It is a 3D LIGHT CONE along the camera's
forward (its pitch included), from the staff (1.2 m over the player, + 0.6 m along the aim): one entry of kind 1 (3D)
in `GameUnitParams::lightCones`, "Staff beam range (m)" (22), "Staff beam half angle (deg)" (4), "Staff beam damage/s"
(30) - each demon in it, or with its body on the axis (perpendicular distance under 2 body radii), is solid and burns.
It costs "Staff beam light/s" (6) of the player's STAFF LIGHT (`GamePlayer::drain`, the shield battery) and stops at
none. **The look is the same cone**: a `SpotLight` with its apex, axis, half angle and range, plus a thin `BeamLight`
core on the axis - that one is given the axis turned 180 degrees about its middle (with the plain axis its tip fade
read as a beam coming toward the camera). The press still selects what is under the crosshair. **The authority's own
player only**: a client's LMB does not reach the server (no intent).

**THE STAFF LIGHT COMES BACK FROM LIGHT** (`GameMatch::updatePlayer` → `GamePlayer::setRegenScale`, x "Energy regen"):
by day as the daylight (`daylight()`), and near a light store - a built own Crystal store or the Beacon within "Hub
reach (m)" (30) that holds light - for the rest, as a building takes light: the authority pays that share from the
store (`store[0]`; a client's own capsule regens there but its server store does not pay). At night away from the
stores it does not come back. The other modes keep the plain regen. **Building has its
own state, the BUILD UI: the cursor is free and is the aim, and the middle button turns the camera** - see The
sandbox world: PLAY and the BUILD UI in [Hotbar.md](Hotbar.md).

**The unit height rules** assume the y = 0 plane: `GameUnitParams::flatGround` (runtime, stamped by the match) is
false here, which drops the height ceiling, the void floor and the bite height window. Barracks units spawn 1 m over
the terrain under their spawn point.

**With "Spawn enemies" on: the waves** (Waves, below). **A wave comes down on
ENEMY LAND** (`queueWave`, the faction layer): out from the Base along a random direction, from "Wave distance
(m)" (70, the nearest) on in 8 m steps, at the first point whose value is under neutral (untouched land or the enemy's) and stays
so for the next step, + 16 m; "Wave max distance (m)" (400) at the latest. So the demons come from farther
away as the players' land grows. With the layer off it is the plain ring at the nearest distance. The bodies appear in
a disc of at most half the nearest distance, 1 m over the terrain, and march to the NEAREST own building that can be
attacked (`waveTargetFrom`: a flying ship's are out of reach; none → the ground under the ship), 6 m short of it. **THE GROUND FAR
FROM THE PLAYERS** has three parts: `groundY` takes the collider's exact height where a ray hits, else the COARSE
GROUND (`GameMatch::setGroundSource`: the App's terrain height from the terrain-data bake, 8 m texels, kilometres
around the camera), else the Base's height; the far tick keeps a far unit `farLift` over that coarse ground (Units.md);
and the terrain collider reaches as far from each physics focus as the SIM LOD keeps a body live (the App passes its
outer radius + the query margin to `TerrainCollider::update`), so a unit that goes live always has a tile under it.

#### The light rules

The sun is the wave clock. `SkyParams::sunHeight()` (Settings: the sun cycle's elevation from "Time of day (h)", valid
the moment the time is written) is the one input; a day's length is "Sky/Sun cycle speed (x real time)".

| Rule | What |
|---|---|
| The start | The authority sets the time to "Start time of day (h)" (7; < 0 = keep it) in `spawnWorld` and switches the sun cycle on. |
| Night waves | `tickWaves`: the dusk edge (`m_night`) counts a night (`m_nightIndex`) and launches the first wave at once; then one wave every `nightWaveInterval()` until dawn - the night's length (from the sun cycle's latitude, day of year and speed) over "Waves per night" (14), held to 5 .. 300 s, so the COUNT of a night's waves is a setting and the sun cycle only says when they come - each from its own random direction. **A night builds up** (`m_nightWave`, `m_waveScale`): see Wave size. "Wave variation" (0.4) is also a random +- on the pause to the next wave. The scale is rolled AHEAD (at dawn for the night's first wave, at each launch for the one after) and is in `nextWaveBudget`, so the HUD's "Next wave power" is the wave that will come. The dawn edge drops the points still queued and sets `m_nightWave` back to 0. |
| Wave size | **Two separate growths** (`nextWaveBudget`): wave k (0-based) of night n (0-based) = "Wave budget" (200) x "Night growth (x)" (1.5)^n x (1 + "Wave growth per wave" (0.2) x k) x random(1 +- "Wave variation" (0.4)). INSIDE a night the waves grow (the last of 14 is 3.6x the first), and that build-up starts again at every dusk; ACROSS nights the whole night grows by "Night growth". **It does not depend on the sun cycle**: a faster cycle, another latitude or day of the year moves the waves closer together or farther apart, never their count, so the wave power is the same at any time speed. `waveLevel()`: the NIGHT number (0-based; by day the coming night) sizes every wave of a night; the recipes unlock by the 1-based night (`minWave` = the first night). **The first night has variety of its own**: a recipe with `WaveArchetype::firstNight` = N also rolls from that night's wave N on (0-based, `m_nightWave`), under its `minWave` - the first wave is the plain swarm, then swarm + runners and the grunt push (1), the skirmish (2), the runner rush (3), the warrior scouts (4). No spitter comes on the first night. `m_waveIndex` still counts waves (GWv, the log). |
| Daylight burn | `stampLightRules` (authority, per frame) writes `GameUnitParams::sunBurnTeam` (the AI team) and `sunDps` = "Sunlight damage/s" (20) plus `sunShare` = "Sunlight damage (share of health/s)" (0.04: of the unit's MAX health, so a big demon burns as fast as a small one - a Titan's 3200 took minutes at the flat rate), both × a smooth step over the sun height 0.02 .. 0.2 (`GameUnitParams::sunBurn(healthMax)` = the health/s now): only with the sun OVER the horizon (the `daylight()` of the fields and the mirrors starts a little under it, and a wave that comes out right after sunset must not burn in the dark). The unit's tick and the far tick both apply it, so a demon nobody is near burns too. |
| Fields | `GameUnitParams::fieldPush` is false: a field does not push a demon. It walks through the shell, takes the graded exposure damage ("Field damage/s", 2: by the depth in the field; a demon sheltered by its own team's bubble takes none) and turns solid and drains the emitter (`addLoad`). "Fields block demons" brings the push back for all; `FieldBlocked true` in a `Component GameUnit` brings it back for one type. Either way only a demon with its OWN bubble is pushed (the emitter's readback); a shield-less one always walks through. |
| Shadow form | `GameUnitParams::shadowTeam` (the AI team; "Demon shadow form" off = none): a demon is SOLID only where the opposing field is at least half the iso value (a little outside the drawn shell), in daylight, or within "Staff light radius (m)" of a PLAYER (default 0 = OFF: a player does not reveal a demon, and carries no field of its own; above 0, `GameUnitParams::lightPoints` is stamped per frame from our player and the client twins) (`GameUnitComponent::solid`, set in its `tickField`). Not solid: its damage inbox is dropped (turret, melee, shells, the player aura), turrets do not pick it, and its model is near-black. Field exposure and the daylight burn write health directly, so they always land. The tint is not replicated: a client sees every demon solid. |
| The field test on terrain | The force bake is ONE horizontal slice of the fields at one world height, made for the y = 0 maps. Two rules make it hold on the terrain: `tickBeacon` moves the slice to 1 m over the Base's ground ("Force/Bake" "Sample height", restored in `~GameMatch`), and a unit inside a field building's bubble by a PLANAR test, any height - the combat probe's `inEnemyBubble` (an emitter within the probe's ~20 m), or one of `GameUnitParams::fieldZones` (the merged bubble spheres of `collectShieldBubbles`, stamped with the SIM LOD zones every 0.25 s: they cover a large bubble's outer part, which the probe does not reach) - is in the field whatever the slice says: full exposure, and solid (`GameUnitComponent::tickField`, only with `flatGround` off). Without them a demon on ground higher or lower than the slice stood in a bubble and read no field. |
| Demon speed | `GameUnitParams::slowTeam` (the AI team) with `slowMult` = "Demon speed (x)" (0.7) and `fieldSlowMult` = "Demon speed in a field (x)" (0.5): `GameUnitComponent::speedScale()` scales the walk speed in the steering and in the far tick - every demon is slower, and slower again while it stands in a player field (`inField`, the field test of the shadow form, without the staff light and the daylight). A field does not push a demon; it holds it back. |
| Demon bubbles | Three enemy prefabs author a `Component Force` and a shield battery: `enemyBulwark.pre` (reach 10, output 1: about 4.5 m), `enemyAegis.pre` (reach 22, output 1.9: about 10 m) and `enemyTitan.pre` (reach 30, output 2.2: about 13 m). A bubble squishes the players' fields at the equilibrium and drains the structures it covers; the bubble is on while the demon is near a player OR anywhere in the BASE AREA (a SIM LOD zone from the Base out to the farthest built structure + 20 m, see Zones in [SimLod.md](../Entity/SimLod.md)); an empty battery collapses it for good: the bubble SHRINKS to nothing over 0.8 s (`m_shieldFade`; the output eases down to 0, which a client follows through the synced output), then the bubble is GONE (`GameUnitComponent::shieldGone`): its emitter is switched OFF (`tickField`, and World's bubble gate keeps it off - no field, no GPU slot), and the demon takes the SHIELD-LESS path of `tickField` (the baked field reveals and exposes it like any body). A heal that refills the battery brings the bubble back. **No floor field** (the player's shield keeps its 0.01): a floor field per collapsed demon added up over a crowd to a real, invisible field that bent the cannon shells and drained buildings. **The battery drains while the bubble pushes into a player field** (`tickField`'s emitter path): "Unit energy drain/s @ pressure 1" (25) x the pressure x the tension - tiny alone, since a pressure is a field value near the iso value (0.01) - plus "Shield drain (battery share/s at iso)" (0.03) x the battery x pressure / iso (up to 4x): about 33 s of pushing at the iso pressure empties any battery, faster when pushed deeper. **A BARRACKS unit drains it too:** each one that stands in the bubble (planar, within its equilibrium radius; the nearest such bubble only, up to 16 m) takes "Game/Friendlies" "Barracks unit shield drain/s" (6) from that demon's battery (`tickCombat` → `drainShield`, the inbox the deflected shots use), so the own army breaks a bubble by walking into it. **While its battery holds, a shielded demon stands its ground:** where the fields push demons (`fieldPush` or `FieldBlocked`), it takes only "Game/Enemies" "Push on shielded units (x)" (0.2) of the push (`tickField`'s emitter path). **A bubble SHELTERS its own team** (`GameUnitComponent::isSheltered`: the baked field where it stands is owned by its team - its team's field is the strongest there and over the iso value; with no bake, its own bubble while its battery holds; one sample a tick, only when asked): a sheltered demon is not revealed by a player field (`setSolid`'s `inField`, and the planar field-zone / probe override in `tickField`, which ignored whose field was stronger), takes no field exposure, and is neither revealed nor burned by a Searchlight. A player field that reaches into a demon bubble but is the weaker one there does not own the point. The staff lights and the daylight still reveal it. Every other enemy prefab is health-only. |
| Solar | `GameStructureSettings::solarDaylight` (the same daylight) scales the solar yield: nothing at night. |
| Palette | The ctor writes "Force/Teams" team 0 (warm gold) and team 1 (violet); `~GameMatch` restores them. |
| HUD | "Sun": `Day N  hh:mm` / `Night N  hh:mm`. "Next wave (s)" shows at night only. |
| A hand-set time | The "Time of day (h)" tweak set by hand counts NO night: a `Tweak::onChange` listener (GameMatch) sets `m_timeOfDayEdited`, and `tickWaves` then takes the sun's new state over and KEEPS THE DAY NUMBER (the HUD's "Day N" / "Night N" = `m_nightIndex` + 1 by day, `m_nightIndex` at night): from day N into the night it is night N (`m_nightIndex` + 1, as at a dusk), from night N back into the day it is day N again (`m_nightIndex` - 1; a dawn would make it day N + 1). `waveLevel()` is then the same on both sides, and so is the wave power. The sun cycle writes the value directly, which fires no listener. |
| Save | `NightIndex`, `Night`, `TimeOfDay` (a load sets the sun cycle's time), `NightWave` (the waves launched this night) and `WaveScale` (the size rolled for the next wave). |

Outside the sandbox world `stampLightRules` writes the neutral values (push on, no burn, daylight 1).

#### The faction layer (sandbox world only)

`FactionLayer` (`Game:Faction`, Faction.ixx / Faction.cpp), a member of `GameMatch`; `GameMatch::tickFaction`, every
instance off its own structure mirror, after the lightning. "Game/Faction" tweaks. The design is in
[`Docs/GDD.md`](../../GDD.md) (Influence: the faction layer).

* **The grid:** 4096 x 4096 cells of "Cell size (m)" (4: a 16 km square), world-fixed, centred on the Base when the
  Base first exists (snapped to whole cells). One byte a cell: 0 = fully the enemy's, 128 = neutral, 255 = fully the
  players'. **All land starts at "Start value" (127: one step UNDER neutral - no tint by day, see The night below)**, and the land outside the grid
  counts as that value (`sample`, and the renderer's `u_terrain_factionOutside`), so the grid's edge does not show. The simulated value is a 16-bit grid, 257 steps a byte step (a byte would stall: a slow step
  rounds to no change); the bytes are its rounded copy, which `sample(x, z)` / `sampleSide` (-1 .. 1) and the renderer
  read. 16 M cells x (2 + 1 source + 1 byte) = 64 MB.
* **Sources** (`setSources`, every frame; a disc, + = the players, - = the enemy): the live Base ("Base radius (m)"),
  every lit field bubble (`m_fieldZones`, its radius x "Field radius (x bubble)"), a wave's spawn site while its
  fog stands ("Enemy spawn site radius (m)", the enemy), and every enemy outpost ("Game/Enemy outposts" "Faction
  radius (m)", the enemy). They are stamped into a signed source field (a byte, -127 .. 127) at the start of
  each sweep: (1 - d / radius)^2 x the strength, a saturating sum. **Only the cells the last stamp and this one touch
  are written** (`m_stamped`: the stamped rectangles) - a whole clear of 16 M cells every sweep would be a hitch.
* **The spread** is a relaxation swept in place over the ACTIVE BOX only - the bounds of the stamped rectangles + 64
  cells (`c_boxMargin`: room for the bleed), set at each sweep's start - (box rows) x dt / "Sweep time (s)" (2) rows a
  frame; **a cell at rest is skipped** (no source on it, the same value all around: most of the box):
  `v += diffusion x (mean of the 4 neighbours - v)`, then under a source `v += |source| x (its side - v) x sweep /
  "Source time (s)"`. Both rates are clamped, so a long sweep cannot overshoot. **LAND KEEPS ITS VALUE:** there is no
  return over time (a return to neutral was built first and removed: it made every cell of held land move every
  sweep). Only a source of the other side takes land back; land outside the box is frozen.
* **The look:** at the end of each sweep that changed a byte, the cell RECTANGLE those changes lie in goes to
  `Renderer::setFactionMap` (a new grid sends all of it: 16 MB, once for each image of the renderer's pair); the
  terrain and what grows on it tint by the map (The faction map in
  [`Docs/Context/RendererVK/Terrain.md`](../RendererVK/Terrain.md)). "Enabled" off clears the grid and the renderer's map.
* **The night:** `tickFaction` passes the shroud's night (`shroudNight`, the value the clouds close by in `tickBeacon`)
  to `Renderer::setFactionNight` every frame. BY DAY only the land the enemy's sources hold (the outposts, the spawn
  sites) has the enemy's look; BY NIGHT all land UNDER neutral has the full look - the untouched land too, which is why
  it starts at 127 and not 128. The players' land (128 and over) does not change. The layer's bytes do not move: the
  step is in the shader, by a live value.
* **Not in the save and not on the wire:** each instance grows its own copy from its sources. The only game rule that
  reads the layer is the wave's spawn distance (`queueWave`).
* **A LOAD** (`loadGame`, co-op) clears the layer, so `tickFaction` makes it again around the loaded Base, and 0.5 s
  later (`m_factionSettleIn`: the field zones refresh first) puts the land under the sources at its END STATE at once
  (`FactionLayer::settle`, computed, not simulated: the pull only stops at 0 or 255, so every cell a source touches goes
  fully to that source's side; diffusion only softens the edge) and sends the whole map. **Until then no wave comes out**
  (`tickWaves`): a fresh layer is enemy land everywhere, so a wave would spawn at the min "Wave distance", which can be
  inside the base. Land that only the old history held (a field that is gone) is not restored.

#### The enemy outposts (sandbox world only)

`GameMatch::tickOutposts`, called from `tickFaction` (so "Game/Faction" "Enabled" off stops it too), every instance.
"Game/Enemy outposts" tweaks. The design is in [`Docs/GDD.md`](../../GDD.md) (Enemy bases and outposts); built so
far: the ring, the building, the beam and the faction source. **No rule yet:** an outpost has no health, spawns no
wave and cannot be attacked or claimed.

* **The ring:** "Count" (8) positions on a regular polygon "Distance from the Base (m)" (600) out, turned half a
  step, made once when the Base exists (`m_outposts`). Every instance derives the same ring from the Base's position;
  nothing is on the wire or in the save.
* **The building** is `Entities/Game/enemyOutpost.pre`: a prop (no game component, no `Component Network`), `Global`
  (it stands outside the SIM LOD), with a static box body and a `Component Force` of team 1 (the AI team): its own
  field of darkness. **It is spawned LATE:** a site's ground height comes from a ray on the terrain collider, which
  exists only near a camera, so a site with no building is probed once a second (`groundY`) and gets its building
  when the ray first hits. Until then the site stands at the Base's height.
* **The beam:** a violet glow-only column from each site (`Renderer::addBeamColumn`: nothing in the light grid),
  "Beam height (m)", "Beam radius (m)", "Beam intensity", "Beam halo range (m)", and "Beam top fade" (the share of the
  height over which the glow fades out toward the top: the beam's tip fade) - 8 of the frame's 32 columns. No
  vortex in the clouds.
* **The faction source:** each site pushes the land within "Faction radius (m)" (350) toward the enemy
  (`tickFaction`), from the moment the ring exists.

#### Lightning (sandbox world only)

`GameMatch::tickLightning`, every instance, after the beacon. "Game/Lightning" tweaks. A LOOK only, rolled per instance
(not synced): no damage, no light on surfaces.

* **When:** while the night (`1 - daylight()`) is over 0.5; the timer runs at the night's rate, and a gap is
  60 / "Flashes per minute" × a random 0.2 .. 1.8.
* **A flash** is one of the clouds' two flash slots (The flashes in
  [`Docs/Context/RendererVK/Clouds.md`](../RendererVK/Clouds.md)): a point inside the cloud layer, "Min distance" ..
  "Max distance" from the PLAYER (uniform by area), "Cloud light" × its level, "Cloud light radius (m)", "Colour". **A flash is 1 to 6 STROKES** (`lightningLevel`, all from the flash's seed; few strokes are the most common): each at
  a random time after the one before (most gaps short, some long), with a random strength and a random decay - one
  flash is a single pop, the next a long stutter. The level is their sum (x a little shimmer); the flash ends when the
  last stroke has died. "Flash time (s)" scales all the times (0.6 = the times as authored).
* **The vortex:** no flash is in the hole. While the hole is open (strength over 0.3), a point that falls inside the
  eye (the radius x the live eye scale), and "Vortex wall share" of all flashes, go to the EYEWALL instead, 10 - 85 %
  up the layer; an arc from there starts at the hole's inner edge and runs down inside the hole.
* **An arc** ("Arc share" of the flashes; slot 0 alone, so one a frame): from just inside the cloud base to the ground
  (`groundY`; the foot leans up to 30 % of the height off the top). 12 glow-only beam columns
  (`Renderer::addBeamColumn`: nothing in the light grid) along a random walk sideways ("Arc zigzag" × its length),
  pulled back to the line at the ground; the walk comes from the flash's seed, so the arc holds its shape while it
  flickers. "Arc intensity" × the level, "Arc radius (m)". **A fork** ("Fork share" of the arcs): 4 thinner, dimmer
  columns off the fifth point, 30 % of the arc's length, ending in the air. The columns draw only with the fog on (Beam
  lights in [`Docs/Context/RendererVK/Lighting.md`](../RendererVK/Lighting.md)); an arc + a fork take 16 of the frame's
  32 columns.

#### The beacon (sandbox world only)

`GameMatch::tickBeacon`, every instance, in `update` before the client branch, off the team-0 Base of the local
structure roster. "Game/Beacon" tweaks. **The cloud hole is the clouds' VORTEX** (see The vortex in
[`Docs/Context/RendererVK/Clouds.md`](../RendererVK/Clouds.md)): its shape (eye radius, arms, falloff) stays in
"Sky/Clouds/Vortex".

* **Once, when the Base exists:** it saves the cloud values it takes (`m_beaconSaved`), sets the vortex "Enabled" and
  its centre to the Base (`Tweak::notifyChanged`: the one shader re-bake). `restoreBeacon` (the dtor, or "Enabled"
  off) writes them back.
* **Every tick the vortex's centre follows the Beacon**, so the hole moves with the ship's Beacon while the ship flies
  (`clouds_vortexCentre` is a live UBO value: no re-bake).
* **Power** (`m_beaconPower`, eased over ~2 s) = the Base field's `outputFrac`; 0 for a dead Base. The energy charge
  is NOT in it (only in the spin): the Base generates only its standing draw, so a store a siege drained stays low
  until the field goes dark, and the hole would stay weak all night.
* **THE IGNITION** (each dusk): the clouds close first, with no hole. "Ignite delay (s)" after the night is full
  (`m_beaconNightSec`) three steps run. THE GLOW ("Beam glow time (s)", `m_beaconGlow`): a short beam (8 m) on the
  Base brightens. THE CLIMB ("Beam climb time (s)", `m_beaconClimb`): its tip goes up to the cloud base, the height
  growing geometrically (slow near the ground, faster in the sky). THE PIERCE ("Pierce time (s)", `m_beaconIgnite`):
  the beam tears through - within the first quarter the hole is at full strength as a SMALL eye, the beam runs out to
  "Beam height (m)" and the light on the clouds comes up - and over the whole time the eye GROWS from 5 % to its size
  (the vortex's live "Eye size scale"; 1 by day). **AT DAWN** the coverage starts to fall at sun height 0, and the Beacon stays
  LIT (`m_beaconLit`) until the shroud is down to "Light off at shroud" (0.5: a lot of sun is through the clouds
  already). Then the LIGHT fades out (the glow runs back over "Beam fade out time (s)": the whole beam dims), then the
  HOLE closes
  CALMLY (the pierce runs back over "Hole close time (s)", its strength fading over the whole time, not the tear's
  quarter); the coverage is back at the day's at sun height 0.25.
* **The shroud's night** (the `night` of `tickBeacon`, not `daylight()`): at dusk (time of day >= 12) the clouds close
  over a sun-height span of 0.2 down to "Shroud full at sun height (dusk)" (0); at dawn they open from 0 to 0.25. The
  night's first wave waits for the ignition (`tickWaves`).
* **Per frame, three LIVE cloud values** (no re-bake): the vortex "Strength" = power × max(the pierce, "Day hole
  strength" × the daylight of the shroud) - the hole follows the ignition, not the shroud; "Spin (deg/min)" = "Calm spin" .. "Pressure spin" by night × the missing charge; the coverage in use
  (`CloudParams::coverageOverride`; the "Coverage" tweak stays the user's) = the clouds' coverage over the day
  (`CloudParams::dayCoverage`) by day, "Shroud coverage" (1) at night (`daylight()`); the restore sets it back to -1. **The night's deck is uniform:** "Coverage
  variation" and "Macro variation" go from their match-start values by day to 0 at night, so the shroud covers the
  whole sky. A panel edit of those rows does not hold while the beacon runs.
* **The beam is TWO records:** the tall beam is the glow in the air alone (`Renderer::addBeamColumn`; its glow fades
  out toward the top over "Beam top fade" of its CURRENT height, so the tip is soft while it climbs too), and a SHORT beam
  at its foot (30 m, `addLightInfo` with no air column) lights the surfaces - the tall beam with its range took
  gigabytes of the light grid. Both: a `BeamLight` from just over the Base's crown (3.2 m over its centre; the diamond floats inside it) up "Beam height (m)": "Beam light intensity" × power, "Beam
  light range (m)", "Beam radius (m)". It lights the ground around the Base and the fog apply draws its column (Beam
  lights in [`Docs/Context/RendererVK/Lighting.md`](../RendererVK/Lighting.md)). **At night only**: its intensity is
power × (1 - the daylight), and none below 2 %. **It also lights the clouds around the hole:** the vortex's "Eye
  light" (a live cloud value; the eye's axis is the Base) = the beam's colour × "Beam light on clouds" × the same
  power × night, 0 while the beam is off. `restoreBeacon` writes the old light back.

#### The fog effects (sandbox world only)

`GameMatch::tickFogEffects`, every instance, right after `tickBeacon`. "Game/Fog" tweaks. Per-frame fog volumes
(`Renderer::addFogVolume`).

* **A wave's spawn site** (`addSpawnFog`, 4 slots, from `queueWave`; clients get the site in `GWv`: index, ground
  point, radius): for "Spawn fog time (s)" a violet fog BANK over the spawn blob ("Spawn fog density (1/m)", with its
  own glow "Spawn fog glow": the night gives fog no light) and a thinner COLUMN from the ground to the cloud base.
  Both fade in over 4 s and out over the last 30 %. **THE SIGNAL** stands in the column: a red, pulsing beam from the
  ground to the cloud base (`addBeamColumn`; "Spawn signal beam glow", "Spawn signal beam radius (m)"). A beam's glow
  is not dimmed by the fog or the distance, so it shows from the Base; red tells it from an outpost's steady violet.
* **The fields clear the fog:** one CLEARING volume (negative density, see `FogVolumeInfo`) per field zone
  (`m_fieldZones`: the merged shield bubbles, at most 16), added AFTER the sites - a clearing volume removes "Field
  fog clearing" of what the list holds before it, the height fog included. A box around the bubble with a soft edge;
  only inside the froxel range (the far field does not know volumes).

**Not built yet (GDD):** damage sources that reach a shadow-form demon, a hole RADIUS that follows the power (the
eye radius is a baked value), lightning, outpost beacons, light beams, crystals and the realistic
resources, the staff. The time of day is not synced to clients.

**Known gaps:** the scene focus, the colliders and the Base site all follow THIS instance's camera, so in multiplayer a
client building far from the server's player has its height trusted (`placeStructure`'s fallback), and the server's
units near a far client have no terrain under them. Client twins joined before the site exists spawn at the anchor.
A lumberyard's / quarry's yield is its placer's survey (the tree records exist only around each instance's own camera). Remote players do not run the strafe animation.

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
so no bubble, battery or GPU readbacks. Built for many-thousand counts. **Every enemy type but the
Titan is health-only** (the barracks' Warrior and Brute included: they share the prefabs).

**Shield-less units interact with emitters through the BAKED PRESSURE FIELD**
(`ForceSystem::sampleBakedField` in the entity pass — a CPU bilinear tap, no per-unit GPU slot,
unlimited counts):

* **The baked field NEVER pushes:** only a unit with its own bubble is pushed (its emitter's readback,
  `GameUnitComponent::tickShieldBubble`). A shield-less body walks through the shell and burns, whatever
  `fieldPush` / `FieldBlocked` say.
* Exposure damage is GRADED by field depth (`smoothstep(0, iso, opposing) × fieldDps × mult`), so the
  burn grows over the shell's fringe instead of switching on at it (`tickBakedField`).
* ~3 frames latent.

**FALLBACK** with the bake disabled: `GameStructureComponent::bubbleRadius` (stamped next to
`strainable` in the death sweep as `reach × 0.5 × outputFrac`) gives the same exposure damage
CPU-side, with no push.

A shield-less body spawns with a **ZERO battery** (`energy = energyMax = 0` when `shieldOutput <= 0`),
so the label pass's shield-vs-health branch and the damage absorb never mistake it for shielded, and
damaged swarm show health bars like any unit. They still deposit `addLoad` strain.

### Waves

At NIGHT only (`tickWaves`: The light rules, above). **`queueWave`:**

1. Picks a RANDOM compass direction and the spawn point on enemy land ("Wave distance" .. "Wave max distance", see The
   sandbox world).
2. **Sizes the wave in BUDGET POINTS**, not unit counts (`nextWaveBudget`: Wave size in The light rules). Each type
   spends its "Cost \<type\>" tweak, **so a brute-heavy archetype fields far fewer bodies than a swarm flood of the
   same budget.** The "Max enemy units" cap (100000, counting every live unit) converts at the cheapest cost, minus
   the points still queued; the clock and the wave index advance even at the cap.
3. Rolls the archetype, sizes the spawn blob, seeds ONE lane, raises the spawn fog and fires "GWv" at clients.

**`tickCoopSpawns` TRICKLES the actual spawns** ("Spawns per frame" 150), so a huge wave enters over seconds rather
than one hitch. An unaffordable roll downgrades to the mix's
cheapest type; if even that does not fit, the wave ends. **The trickle is THREADED:** the frame's
rolls (budget math plus glm RNG, main-thread — `linearRand` is not thread-safe) collect into
`NpcSystem::spawnLooseUnits`, one `World::spawnBatch` fan-out per frame, with team/order fixup serial
after the join.

### The spawn blob

Bodies land in a disc around `m_waveOrigin` (the spawn point), **sized ONCE per wave in `queueWave`
by `waveSpawnRadius`: the AREA scales with the wave's expected body count, so the areal density is
the same in wave 1 and wave 20.** Body count = the queued points / the MIX's weighted mean cost, and
`r = sqrt(bodies · "Wave spawn area per unit" (6 m²) / π)`, clamped to [8 m, `c_waveRadiusMax` 300 m]; the trickle
holds the disc to half the "Wave distance", so it never reaches the Base, and puts each body 1 m over the terrain. It
rides the save (`WaveTrickle/Radius`), so a mid-wave load keeps the same blob.

> Do not size the radius off the REMAINING budget: parked bodies never push each other apart, so a stacked disc only
> resolves as box3d shoves them out once a player comes near, and a shrinking disc packs the wave's tail tightest.

Spawn points then roll up to 6 times against the last `c_waveRecentSpawns` (32) spawns at 2 m
spacing, **so parked bodies rarely overlap in the first place.**

Each unit is `orderMove`d to the nearest own building on the incoming side (`m_waveDest` = `waveTargetFrom`, ±4 m jitter);
the lock releases on arrival OR at the first enemy structure inside "Order break radius" (15 m —
`GameUnitParams::orderBreakRadius`, checked by the unit's combat probe), and the AI takes over,
hunting the NEAREST structure — without the break the whole wave marches past everything to the Base.
**THE RAIDERS** (`GameMatch::tickRaiders`, authority, every 1 s): the AI looks only within "Target search radius" (the
range gate below), so a unit that broke an outlying building stood there when the next one was farther. Every AI-team
unit with NO order (`targetLocked` off: a wave's after its march, a Spawner's brood) whose nearest standing own building
(`waveTargetFrom`'s rule) is more than that radius + 2 m away gets a new march order to it (6 m short on its side, +-2 m
of jitter); the order breaks and the AI takes over there again. So a wave eats its way through the base.
All spawns are loose units — **sourceId 0: no route, no population accounting.** The authority HUD
shows "Next wave (s)".

#### Unit types and costs

`ENpcType`: Grunt, Brute, Runner, Spitter, Swarm, Elite, Giant, Titan, Lobber, Spawner, Warrior, Bulwark, Aegis.
Costs are the "Game/Coop" "Cost \<type\>" defaults.

| Type | Cost | Notes |
|---|---|---|
| Grunt / Brute / Runner / Spitter / Swarm | 3 / 25 / 2 / 10 / 1 | The barracks tier. Spitter is enemy-only. Grunt, Runner and Swarm are HEALTH-ONLY bodies (no Component Force). |
| Warrior | 5 | `enemyWarrior.pre`: the tough grunt (80 hp, harder hits, a bit bigger and yellow) — a BARRACKS option too (12 energy, 3 pop); "warrior line" from wave 4, "shield wall" from 6. |
| Elite / Giant / Titan / Lobber / Spawner | 15 / 100 / 500 / 30 / 40 | **Enemy-only elite tier**, `enemyElite/Giant/Titan/Lobber/Spawner.pre`. |
| Bulwark / Aegis | 8 / 50 | **Enemy-only SHIELDED demons** (a bubble + a shield battery, see Demon bubbles): `enemyBulwark.pre` (35 hp, 150 battery, a 4.5 m bubble, violet; the sprinkle from night 1 like the Warrior) and `enemyAegis.pre` (150 hp, 700 battery, a 10 m bubble, slow, siege bite 80, dark indigo; the sprinkle from night 4 like the Spawner). |

* **The Lobber** is `Ranged` with `ShotKind 1` = the slow SPLASH shell `enemyLob.pre`
  (`SplashRadius 3`, 12 unit / 18 structure damage); "Lobber shot speed" 14.
* **The Spawner** is a tough ranged-stance hive: `ShotKind 2` makes each "shot" a LOOSE Swarm body
  born beside it toward its target every `FireInterval` while it holds at `StandoffRange`. **The
  births are loose units — no wave budget, no cap — so the hive itself is the thing to kill.**
* `isBarracksUnitType` allows only Grunt / Brute / Runner / Swarm / Warrior; **an old
  Spitter-barracks save loads as a Grunt barracks.**

#### Archetypes (`c_waveArchetypes`, [MatchCoop.cpp](../../../Code/Game/Private/MatchCoop.cpp))

ONE archetype per wave: a named recipe of weighted types (up to 5 — "endgame"), **gated by a minimum
night** (`minWave`, 1-based), weights jittered ×0.6–1.4, and **never the same recipe twice in a row** when a choice
exists.

| Min night | Recipes |
|---|---|
| 1–3 | swarm · swarm + runners (2) · grunt push (2) · brute pack (2, the first Brutes) · runner rush (3) |
| 4–6 | warrior line (4) · spitter siege (4) · brute hammer (5) · shield wall (6) · combined arms (6) |
| 7–8 | brute wall · **elite guard** (7) · the works · **lobber barrage** (8) |
| 9–10 | **giant push** · **hive** (9) · siege column (10) |
| 11–13 | **titan** (11) · **hive siege** (12) · endgame (13, all four elites) |

**THE SPRINKLE** (`queueWave`): each wave adds ONE strong type on top of its
recipe, with a weight that gives it 15 % of the wave's points (`c_sprinkleShare`; the mix weights are per body, so
the weight is `share * sum(w * cost) / ((1 - share) * cost)`, the `addShare` lambda). Candidates by the night (1-based):
Warrior 1, Bulwark 1, Brute 2, Elite 3, Spawner 4, Aegis 4 (`c_rareChance` 15 % of the waves from a type's rareLevel,
every wave from its minLevel; all six have the two equal now; the table is by rising cost) — only a type the recipe
does not have, and only when the share pays for half a body or more. The roll is between the two most expensive
candidates, so from night 4 it is the Spawner or the Aegis.

**THE HEAVIES** (`queueWave`, after the sprinkle; `c_heavies`): Giants and Titans, on top of the recipe and the sprinkle
(not when the recipe has them already). From its MAIN night a heavy is in EVERY wave with its share of the points, one
body at least: **the Giant from night 5 (20 %), the Titan from night 6 (15 %)**. The night before, a SCATTERING: in
30 % of the waves (`c_scatterChance`) a few bodies' worth of points - 2 Giants on night 4, 1 Titan on night 5. A share
over 50 % (a wave too small for the body) is skipped.

**THE BODY CAP** (`queueWave`, last): the points grow x "Night growth" a night, so late nights would field hundreds of
thousands of small bodies. The bodies a wave may buy = "Target enemies alive" (20000) less the demons alive now, held
to 1/8 .. 1 x "Wave max bodies" (2500). When the queued points (`m_wavePendingBudget`, a tail included) would buy more
with the mix, **the mix shifts to the costly types**: every weight x cost^a, a (0 .. 4) found by bisection as the
smallest that brings the mean cost per body to points / bodies - the recipe keeps its character, the elites take its
points. First the ELITE POOL the night has unlocked joins at weight 0.01 where missing (Brute 2, Elite 3, Aegis 4,
Giant 5, Titan 6, Lobber 7), so a swarm recipe has something costly to shift to. At a 4 the shift is at its most; the
"Max enemy units" cap (40000) stays the last guard. The Hive's births are loose units outside every budget.

The sprinkle, the heavies and the pool are entries of `m_waveMix` (capacity 14: a recipe's 5 + the sprinkle + 2
heavies + the pool's 6), so the save holds them.

`sampleMix` samples the wave's stored jittered copy per spawn (its own SplitMix64 roll: `glm::linearRand` never reaches
the top ~0.4 % of its range, so the mix's last, rarest entry was never rolled). The wave log names the recipe.

### Targeting range gates

EVERY unit's Nav team-field targeting is RANGE-GATED by "Target search radius" (15 m, geodesic). The
field rebuilds every Nav "Rebuild interval" with LIVE source positions, **so in-range units track a
moving player tightly while out-of-range units hold their patch instead of marching across the map.**

Out of range, `hasTarget` from an earlier tick is **DROPPED at once** — kept, the unit would march to
the last known spot for up to a whole retarget interval. Inside the "Nav follow radius" (40 m) band
the unit may still walk an existing crowd lane.

Wave units stay order-driven until arrival or the first enemy structure inside "Order break radius",
then the same gated AI takes over - and with no building in its range, a new order (THE RAIDERS, above).

**That standing order is therefore the ONLY thing that marks a wave unit**, so the F9 save carries it
per unit (`Order`, see Save / load ([CONTEXT.md](CONTEXT.md))): without it a loaded wave stops where it
stands and holds its patch.

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
* **Territory:** `GameStructureComponent::update` drains "Damage/s in enemy field" (3) health while
  ANY other team's bubble owns the structure's position (`sampleBakedField`), **x the depth in that bubble**: 0 at the
  shell, 1 where its field is twice the iso value (`field / iso - 1`). So the edge of a small demon bubble (the
  Bulwark's) barely touches a building, and the core of a Titan's drains at the full rate. The drain goes through
  `damage(amount, retaliate = false)` (`fieldDrain`): the wards take it, but a Retaliate ward sends back (and pays for)
  nothing, since a field has no biter. *Push your field over their buildings to siege.* `StructureSystem::tickDamage`
  is only the death sweep.

**Known gaps:** player turrets target only units, not players; GSt caps at 79 structures.
