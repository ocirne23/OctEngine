# Rivers

> Part of the Procedural documentation ([`Docs/Context/Procedural/CONTEXT.md`](CONTEXT.md)) — read that first.

"Terrain/Rivers" and "Terrain/Rivers/Surface" tweaks (Settings.Terrain, `river*`). All in `Private/Rivers/`. River
config values are MODEL-frame: model metres (/ 6 in the world at mpp 5), native px, real m3/s.

Three levels: the COARSE network decides where water crosses tile edges, the UNITS route it at native resolution, and
the SAMPLER (`RiverTerrain`) carves it into the terrain. `RiverSystem` draws the water.

### Rivers: the coarse network (`:RiverNetwork`)

**One coarse pixel IS one full tile** (256 native px each), so a coarse flow direction is the direction water leaves a
full tile.

`CoarseRiverNetwork(generator, RiverConfig)` → `tile(ti, tj)`: per coarse tile a `CoarseRiverTile` (D4 receiver,
outflow Q in m3/s, catchment in coarse pixels, filled height, `ERiverWater` Land / Sea / Lake / TerminalLake / Pan,
lake level, `runoff`). **Each tile is routed over its OWN domain** (the `(2R+1)^2` coarse tiles centred on it, "Coarse
domain (tiles)"), so the result never depends on the order tiles were asked for; neighbours can disagree only on rivers
whose catchment is bigger than the margin. Steps (`RiverNetwork.cpp`):

* **Sea** = deeper than "Sea depth (m)", plus below-zero ground connected to it - V3's -0.05 m film is land.
* **Priority-flood** (with the FIFO pit queue for flats) from the coast and the domain edge: a D4 receiver and a filled
  height per pixel; the pop order is downstream-first.
* **Runoff** = rain - evaporation on the Budyko (Fu) curve ("Evaporation curve", "Evaporation per C (mm/yr)"), from the
  tile planes (`fetchCoarseTilePlanes`: raw precip mm/yr, tempSea + the config's lapse and temperature offset, the
  humidity offset as a precip offset - the climate the biomes show).
* **Depressions** deeper than "Breach depth (m)" and at least "Lake min cells" are lakes - and so is ANY depression
  deeper than 2 x the breach depth, however small (a pond; the units apply the same rule with their "Unit" values).
  **Cutting through a deep small basin makes a gorge tens of metres deep** that floats or canyons across the unit edge.
  The rest pass water through.
* **THE SIZE LIMIT** (units only, "Unit lake max cells", default 20000, 0 = none): the level goes between the basin's
  LOWEST that many pixels and the next (a terminal lake's hypsometry), and of the pools under it the LARGEST is the lake;
  the other pools and everything above are land again - the water runs over them as channels, and the outflow's breach
  profile cuts down through the rim to the lower level. **The basin's drain tree is rebuilt** (`capDepression`): ONE
  path leaves (from the lake's lowest pixel, from where it last leaves the lake) and every other basin pixel drains into
  the lake or onto it (a priority flood from the path, lowest first). The flood's original tree, run from the spill
  through the whole basin, gave rivers that started and ended in the same lake. The basin keeps its rank slots,
  reassigned in the new tree's order.
* **THE EDGE CAP** (units, `edgeHoldsNoLake`): the neighbour unit cannot see a unit's lake, so a lake over or beside a
  grid-edge pixel (the unit's edge or a missing tile's) would stand in the air where the neighbour's ground falls away.
  Its level stays 0.1 model m under the lowest such pixel; only the pixels under that stay lake, through the same
  `capDepression` (none left = no lake, the river runs through the basin).
* A lake gathers its inflow and releases at its exits when the last one is in: full (spills inflow + own rain -
  open-water evaporation, "Lake evaporation"), terminal (the lowest cells that evaporate exactly the inflow, no
  outflow) or a salt pan (under one cell). Dry land loses channel water by aridity ("Dry channel loss":
  `loss x (PET/P - 1) x km x sqrt(Q)`).

Not disk-cached: the input coarse tiles are, and a domain routes in milliseconds. The planes are cached per network
object. **`TerrainPreview` draws it** ("Show on preview": rivers by Q, lakes, brackish terminal lakes, pans, dry beds),
and a "Terrain/Rivers" change re-colours the published map from its stored samples, with the map kept up.

**`:RiverRouting` is the router both levels run** (`routeDrainage` over a `DrainageGrid`: optional mask, D4 or D8,
caller seeds, inlet injections; `markSea`; `riverPixelClimate`). A masked region no seed reaches drains to its own
lowest pixel. The coarse level runs it D4 (a coarse link is a tile edge); the units D8.

### Rivers: the units (`:RiverUnits`, `:RiverSystem`)

**A unit = N x N full tiles** ("Unit tiles", default 6) on a fixed model-space lattice, routed at native resolution by
`buildRiverUnit` (`RiverUnits.cpp`).

**The unit edges.** The coarse network decides where water crosses an edge; the fine routing decides where it goes.
Every rule below exists so the two sides of an edge agree without seeing each other.

* **Crossings** = the coarse links across the unit's edge (or into one of its missing tiles). The point is the low
  point of `min(this tile's border, the neighbour's border)` (box-smoothed, inside "Crossing window (px)" of the edge's
  middle) - **both units read the same two tiles, so they pick the same point**. Q = the coarse link's, water level
  = bed + depth(Q). Outlets are flood seeds, inlets inject their Q. A tile whose coarse water ends in it (dir None)
  with no native sea drains to its lowest pixel.
* **The two sides of a crossing MEET**: the inlet's segment starts and the outlet's segment ends on the shared point
  on the tile boundary between the two border pixels, with the crossing's Q; the outlet side blends its own fine Q into
  the crossing's over its last 24 px (`c_crossBlendPx`; width, depth and valley follow Q) - only where the fine Q is
  within half to twice the coarse one (from far off, a small stream swells into a big river at the edge, or a big one
  shrinks to a creek) - and ends at or below the crossing's level. **Direction**: after the path shaping, the stretch
  within 16 px of the crossing is pulled onto the edge's NORMAL through the boundary point (fully at the point, fading
  out by smoothstep), so both sides meet in one direction.
* **Every other edge pixel is a SOFT wall**: a seed ordered "Edge wall (m)" above its ground (`DrainageSeed::priority`),
  so water leaves there only when every way to an outlet climbs more, and its segment ends `ERiverEnd::Edge` (the
  neighbour does not continue it). **A basin that spills through a soft seed is never a lake**
  (`DrainageResult::root`). Hard walls fill valleys whose fine watershed crosses a unit edge up to the lowest pass:
  lakes far above the hillside, cut off at the edge. **No soft wall within 32 px of an inlet** (`c_inletGuardPx`), or
  an arriving river runs straight back out beside where it came in.
* **THE EDGE REROUTE**: an OUTLET whose fine water falls under half its coarse Q takes the largest SOFT EXIT on the
  same unit edge within 3 tiles carrying at least 30 % of that Q: a least-climb Dijkstra inside the unit (within 48 px
  of the edge, off the border pixels, not through the sea or a lake; cost = the step x (1 + 0.5 per model m above the
  exit's ground)) carries the exit's water on to the outlet - receivers and outflows along it rewritten, its pixels
  given the exit's rank. Each exit is used once. **Not past a rim of 2 x "Unit breach depth"** (routing never cuts
  deeper; there the fine and coarse drainage really disagree). The breach keeps the exit's level through the rim (a
  gorge), so the water reaches the crossing LOWER than the crossing's own level; the sampler's INLET MATCH (below)
  carries it on.
* **THE COAST**: in a tile whose COARSE pixel is sea, every pixel under 0 is sea too (`markSea` alone seeds only below
  "Sea depth", so a shallow coastal tile has no sea and the fine river finds no water where the coarse one ends).
* **THE SEA RESCUE**: a soft exit still carrying at least "Sea rescue min Q (m3/s)" (5; 0 = off) - water leaving where
  the coarse network has no crossing, which the neighbour never carries on - is sent along the LOWEST-RIM path inside
  the unit to a sea or lake pixel.
  * **The direction comes from the coarse network**: from the exit's tile its flow directions are followed tile by
    tile to a coarse Sea / Lake / TerminalLake tile in the unit, and the fine search keeps to that route's tiles and
    their 8 neighbours (a CORRIDOR). A route that leaves the unit or ends in a dry coarse sink means no rescue.
  * **From the river's MAIN STEM, not its exit** (started at the exit, the river runs on to the border and turns back:
    a dead end at the edge). Every pixel upstream from the exit along the largest inflow (while it carries at least half
    the exit's water, up to the max length) is a start; the key is the highest ABSOLUTE ground on the way (whole model m
    + an offset, x 1e4, + the length, in doubles), so the branch is wherever the lowest ridge is reached soonest. The
    stem below the branch loses the branch's water.
  * Never along the border or off the present tiles. A rim more than "Sea rescue max rim (m)" (300) over the BRANCH's
    ground, or a path longer than "Sea rescue max length (px)" (1500), means no rescue.
  * **The path wanders**: past the ridge every way has the same key and the plain shortest one is long straight D8
    runs, so a step's length costs x (1 + 1.5 x a value noise over 24 px cells, in global pixels).
  * Receivers / outflows / ranks rewritten as for the edge reroute. Not covered: a basin that spans a unit edge (a
    cross-edge lake is not built).
* **An inlet on a sea pixel starts a segment too** (`Sea` at once, with the inlet's Q, then the sea channel): the sea is
  marked per unit, so one crossing can be land upstream and sea downstream.

**Water budget and channels.**

* **Each tile's land runoff is rescaled to its coarse pixel's own** (`CoarseRiverTile::runoff`), so the levels agree on
  the budget.
* **Channels** (Q >= "Channel min Q", outside lake basins) become segments from heads / junctions / inlets to a
  junction, an outlet, the sea, a lake basin's rim, a sink or dry (losses took the water). **THE GROWTH**: a channel's
  width and depth rise smoothly from 0 at "Channel min Q" to the hydraulic geometry ("Width a" x Q^0.5, "Depth c" x
  Q^0.4) at min Q + "Fade Q", so a stream fades in at its head and out where its water drains away - the ribbon, the
  carve and the influence all follow. A segment never above "Perennial Q" is ephemeral (a dry bed).
* **The profile: UPSTREAM first, pure breach semantics.** A segment starts at the lowest of its own bed + depth, the
  water its tributaries bring and (an inlet) its crossing's level, then W = min(W above, bed + depth) - it never
  rises, so a rim in its way is CUT (the carve's gorge), never filled. Only the sea (0) and a lake (its level) floor it.
  **Never floor a segment at the level its downstream segment starts at**: in a filled basin that is the rim, and whole
  rivers stand at rim height over the basin floor. An outlet crossing's level does not floor the upstream side either,
  so the two units can meet at slightly different levels at a crossing.

**Path shaping**, in this order:

1. **Smoothing**: the D8 path is resampled at 1 px of arc length and Gaussian-averaged along its length ("Path
   smoothing (px)", sigma, default 10); the window shrinks symmetrically toward the ends, so junctions and crossings
   stay put.
2. **Meanders** (`meander`): a sideways swing, two sines over an ACCUMULATED phase - wavelength "Meander wavelength" x
   the channel width (it follows the width as it changes; at least 4 px), swing "Meander amplitude" x the width; small
   streams x "Meander small wavelength" / x "Meander small amplitude" at "Channel min Q", blending (log Q) to x 1 at
   "Meander full Q"; phase from the segment's start in global pixels; faded to 0 within half a wavelength of each end;
   shrunk toward 30 % as the water steepens past "Meander slope"; KEPT ON THE VALLEY FLOOR (the largest of 1, 3/4, 1/2,
   1/4 of the swing whose ground stands at most the channel's depth above the point's own). **The steepness factor and
   that pull-back are BOX-SMOOTHED over half a meander wavelength** (8..64 px): per sample, the breach profile's
   staircase flips them and makes Z-shaped jogs.
3. Chaikin x2.
4. **THE GROUND CLAMP** (`clampToGround`): the shaping moves the path with W tied to its arc position, so W can stand in
   the air over lower ground (the carve only lowers). On the final path W <= the ground under it + the channel's depth,
   then a running min downstream again - **never under the profile's floor** (a `Lake` end's lake level, a `Sea` end's
   0), or the lake's sheet stands over the river.
5. Douglas-Peucker that keeps a point when EITHER its plan offset passes 0.1 px OR its W passes 0.25 model m from the
   interpolation - **W is linear between the kept points**, so a plan-only simplify lets the water slope through the air
   over a drop in a plan-straight reach.
6. **THE WATERFALLS** (`shapeFalls`, on the FINAL points after the simplify, which would spread a step again): a run of
   pieces steeper than "Fall slope", at most "Fall max length (px)" x clamp(Q at the lip / "Fall full Q"), dropping at
   least "Fall min height (m)", becomes a straight drop at its TOP - the water keeps its level to the lip, drops over
   0.6 px (`c_fallLipPx`, a second point) to the run's lowest level and runs on at it to the run's end. The lip's top
   point carries `RiverPoint_Lip`; the foot and the run's points but its last `RiverPoint_Gorge`, so the carve cuts a
   NARROW plunge gorge back into the slope. A longer steep run stays sloped rapids - WHOLE (shortening its tail makes it
   fall halfway down the slope, the gorge cut into the hillside).

Points carry W, half-width, depth, Q and rapids / fall flags (by the W slope).

**Segment ends.**

* **THE END LAKES**: a perennial segment ending in a `Sink`, running `Dry`, or reaching a soft wall floods a TERMINAL
  LAKE at its end ("End lake size (px per m3/s)", at most 20000 px; 0 = off). An `Edge` end is first pulled back 8 px
  from the border (`c_edgeBackPx`) and becomes `Dry` (the neighbour does not see it). The flood starts at the last pixel
  and takes the LOWEST GROUND FIRST, so the lake has the terrain's own shape. It never goes into the edge band (2 px),
  the sea or another lake; those are only its shore.
  * **HELD**: the flood rose over a rim with every shore pixel above its level. The lake fills to there; a held size of
    at least half the target counts.
  * **Else CARVED** (the flood ran down a slope or out into the edge band / the sea). Its SHAPE comes from a second
    flood with cost = the ground + 1 model m per px from a CENTRE + an 8 model m value noise over 8 px cells (lowest
    ground alone gives a straight shore on an even slope). The centre is the end moved off the unit's edges by 1.5 x
    the round radius + 12 px. Its pixels sit 0.1 model m under their lowest shore pixel (`RiverLake::carved`), and
    RiverTerrain digs their ground to 3 model m under the level (`c_carvedLakeBedM`). **Never above the arriving
    river's water** (the breach profile can run the river in a gorge far under the ground the flood saw): a level over
    the end's water drops to it, carved. Not when the digging is deeper than "End lake max depth (m)" under the highest
    pixel (then the river fades out).
  * **Another river's channel ENDS the flood** (only channel pixels that drain to this end, or its own cut-off tail,
    join; the lake is then held or carved at what it has).
  * At least 16 px, or no lake.
  * **Every segment ends where it first enters an end lake** (end `Lake`; sampled along its pieces every half pixel,
    because the simplified points are sparse; a segment that starts in one is gone).
  * **A lake cannot cross a unit edge**: two units that end rivers at the same border point make two lakes.
* **THE LAKE CHANNELS**: a `Lake` end runs on into the SAME lake down the steepest way (8 neighbours, never back) until
  the raw ground is "Lake channel depth (m)" under the level (12) or "Lake channel max length (px)" (40), at the lake's
  level and the arriving Q, so the carve cuts through the shore into the bed (the drawn ground stands a little above
  the level at the shore). **The same for a river LEAVING a lake** (`lakeHead`): the chain runs BACK into that lake,
  prepended, at the lake's level with the start's Q. The ribbon on those stretches lies under the lake's surface.
* **THE SEA CHANNEL**: a `Sea` end runs on into the sea down the steepest way (sea pixels only, never back) until the
  floor is "Sea channel depth (m)" deep (60, ~10 engine m; 0 = off) or "Sea channel max length (px)" (200); its Q is the
  mouth's, its level falls from 0 to minus that depth, so the carve cuts a channel out to deep water and the ocean fills
  it (a river meeting a near-0-deep sea on a coastal flat otherwise seems to stop). RiverSystem's ribbon stops where the
  level drops under -0.5 model m (`c_seaChannelCut`): the ocean is the water there; `apply` does not mark such points
  River.

**Lakes** are stored as wet-pixel row runs plus a level and kind per lake.

**Disk cache** `Local/Diffusion/<seed>/river_x<j>_z<i>_n<N>.rvu`, keyed by a hash of every river setting and the
generator's climate shaping. **The key does not cover the code: a change that moves `RiverUnits` output must bump
`RIVER_UNIT_VERSION`** (RiverUnits.cpp). **Only a unit whose tiles and edge neighbours all lie inside the generated
bounds is cached** - past them the result depends on which tiles exist.

#### `RiverSystem` — the water

A TerrainStreamer MEMBER (no global): `rebuildMaps` hands it the live `RiverTerrain` (`setTerrain`), and `update` runs
right after `updateTerrainTextures` (after beginFrame: it pushes nodes). One Low job (`"RiverSystem::pullUnit"`) pulls
the nearest missing unit inside max("Surface/Radius (m)" while the water is on, "Debug radius (m)" while the lines are)
from the sampler's store (built there if not resident); units past 1.5 x the radius go. A "Chunk size" change or
toggling "Surface/Enabled" re-pulls the units.

* **The water meshes** (`buildSurfaceMeshes`, pure, on the pull job): one WATER TILE per terrain BASE CHUNK the unit has
  water in (a ribbon quad goes to its midpoint's chunk, a lake run is cut at the chunk columns):
  * a RIBBON per perennial segment at the carved water surface (`RiverCarveConfig::surface`), 1.2 x the channel's
    half-width (`c_ribbonWiden`; the carved bank hides the rest);
  * a QUAD per lake row run at the lake's level, one pixel wider all round (the ground cuts the shoreline; pans have
    none).
* **`segmentWater`** (shared by the ribbons and the near cells, so the two lie on one another): per water point the
  carved surface, the half-width, the flow and the WHITEWATER.
  * Flow speed: Manning-like (capped at 6 m/s); on a fall-steep piece blended toward 0.7 x the free-fall speed
    sqrt(2 g h) over its drop, up to 9 m/s; each point raised toward any faster water downstream within 30 m (water
    speeds up toward a lip); then smoothed along the river by DISTANCE (Gaussian, sigma 20 m), only ever raising (an
    average gives a fall's short sheet the calm water's speed).
  * Whitewater: a smooth measure of how steep the carved water runs (0 below half "Rapids slope", 1 at "Fall slope",
    eased; blurred along the river) - not the rapids / fall flags, which only colour the debug lines.
  * **At a JUNCTION** the river flowed into stands lower (its bigger Q: a deeper channel and valley sink), so a segment
    ending there EASES DOWN to that river's surface over its last 3 x the main half-width (at least 20 m), and its flow
    direction, speed and whitewater blend into that river's first piece's. A clearly smaller one (under 0.6 x the main
    half-width: a tributary) STOPS 0.85 x the main half-width before the junction, just inside the main water. **The
    LARGEST arriving branch is only eased, never cut** (it is the main river's continuation; cutting both of two similar
    streams leaves the junction's upstream side dry).
* Main uploads each tile (`createMesh`, no BLAS) and spawns its node at the unit's origin at sea level, `PASS_MAIN` (0
  while the tile is past "Radius"). Material `RenderMaterial::createForMesh(River, false)`: not in the TLAS. Drawn by RendererVK's
  River variant (RendererVK "River and lake water").
* **IT RIDES THE TERRAIN'S CULLING** - no spatial entry of its own (one multi-km mesh per unit is never culled). The
  tiles are listed per base chunk (`m_tilesByChunk`); TerrainStreamer's hand-over walk lists every node it drew in the
  main pass (live: entry valid; `firstBase` + span 2^lod) and at its end calls `RiverSystem::drawChunks` ON ITS WORKER,
  which pushes the tiles over them (once each: a LOD handover's two residents). Water is drawn exactly where its ground
  is. The tiles only change in `update`, which TerrainStreamer calls after joining the walk, so nothing is retired. With
  culling off `update` pushes every tile itself.
* **The near cells** (the waves' geometry): 64 m squares (`c_nearCell` = the shader's `RIVER_NEAR_CELL` - **keep them
  in step**) whose centre is within "Near radius (m)" + one cell, each ONE dense mesh of every perennial river crossing
  it (`buildNearCellMesh`: each segment's `segmentWater` resampled every "Near spacing (m)", "Near across" vertices
  wide; a row pair belongs to the cell its first row lies in), built nearest first on their own Low job
  (`"RiverSystem::buildNearCell"`) from the resident units near the cell, one node per cell at its origin, `PASS_MAIN`,
  drawn with its chunk as a tile. A cell also holds a dense LAKE grid: every spacing a vertex where a wet lake pixel lies
  within one pixel, at that lake's level, a quad where all four corners are in - the VS gives it the lakes' waves. Each
  frame RiverSystem reports the NEAR COVERAGE (`Renderer::setRiverNearCovered`: the nearest missing cell's centre less
  half its diagonal, at most the radius); the light ribbons and flat lake quads discard inside it. A unit adopted or
  evicted bumps the unit generation: every cell goes STALE and keeps its mesh until its rebuild lands; a near-mesh
  setting change drops them.
* **Debug lines** ("Debug lines"): channels blue by Q, ephemeral tan, rapids orange, falls red, the channel's edges
  white and the carved bed's outer edge (half-width x (1 + bank + floodplain)) dim green, outlet / inlet ticks magenta /
  green, lake row hatching.
* **The mist sources** (`updateMist`): the unit pull job collects, with the water mesh, every water point with any
  whitewater (`MistPoint`: surface point, channel half-width, flow, whitewater, channel depth, the stretch it stands
  for). Each frame the resident units' points within "Mist radius (m)" go to `Renderer::setRiverMistSources` (nearest
  first past `MAX_RIVER_MIST_SOURCES`; re-sent after 10 m of camera motion or a unit change), their whitewater x the
  river's size (depth / "Full size depth (m)", weighted by "Mist size weight"). The renderer's producer spawns
  `Effects/river_mist.pfx` over them: **Procedural cannot link Particle**, so the mist goes the ocean spray's way.
  **A FALL'S FOOT** mists too (the whitewater mists only over the lip's few metres): the water point under a drop of at
  least 2 m, steeper than 45 degrees, is a plunge source (`MistPoint::plunge` = the drop) at full whitewater, its
  stretch "Mist plunge" m per m of drop (0 = none), centred a third of it downstream - applied in `updateMist`, so the
  tweak works live. Its half-width goes up NEGATIVE: the shader then takes "Plunge mist centering / speed / kick (m/s) /
  height (m)" in place of the whitewater's four.
* **The inland water map** (`updateWaterMap`): `RIVER_WATER_MAP_DIM`² texels of `RIVER_WATER_MAP_TEXEL` m around the
  camera, each the inland surface Y at its centre (`RiverTerrain::sampleInlandWaterGrid`: the river's calm carved
  surface inside its channel, else a lake's level; units only, no terrain; the KIND in the height's lowest mantissa bit,
  1 = a river channel, for the fog's "River underwater scale"), baked on a Low job (`"RiverSystem::bakeWaterMap"`) when
  the camera has moved `c_waterMapMove` (40 m) from the last bake's centre or the unit generation changed, handed to
  `Renderer::setRiverWaterMap`. RendererVK's underwater fog and terrain wetness read it. **The buoyancy reads it too**:
  RiverSystem keeps the last bake's heights (`sampleWaterHeight`, nearest texel, -FLT_MAX off the map or on dry land;
  `TerrainStreamer::sampleInlandWaterHeight` / `hasInlandWater` forward it), swapped on main in `update` after the
  entity pass has joined, so the workers' reads on the next pass are const. The App takes the higher of it and the
  ocean's surface. No waves - the calm carved surface / the lake's level.

### Rivers: the sampler (`:RiverTerrain`)

**`RiverTerrain` is an `ITerrainSampler` that WRAPS the `TerrainGenV3`** - a wrapper, not a part of the generator,
because the units are built from the generator's tiles. `rebuildMaps` publishes it as THE maps when
"Terrain/Rivers/Enabled" (default on), so the nodes, the collider, both bakes and the tree / rock / clutter records
sample the carved terrain with no change of their own. Every river tweak except the preview / debug ones is a streamer
config-dirty listener (a rebuild; the units then reload from disk).

**The unit store.** `unit(ui, uj)` builds (or loads) on first touch, BLOCKING like a tile miss; concurrent requesters
park on the unit's `JobEvent`, one builds; FIFO-evicted past 256 (`c_maxUnits`). `resolve` takes every unit within the
carve reach of the query rect ONCE (sampleGrid: per unit, not per point), skipping units with no tile in the generated
bounds.

**PREPARE** (`prepare`, on load; on a COPY - the store's unit and the disk cache stay as built), in this order:

1. **THE INLET MATCH** (`matchInlets`): a segment starting on the unit's border looks up the upstream unit's segment
   ending at the same boundary point in its EDGE SUMMARY (`unitEdges`: per unit only its outlet ends - position, level,
   half-width, depth, q - from its raw unit as built; units build independently, so this never chains. Building a
   summary builds the raw unit, which waits in a small HAND-OFF (16 units) for that unit's own `unit` call, so it is
   built once).
   * It holds its water, and the segments it flows on into, at or below that level: where the upstream water arrives
     lower than the crossing's level (the edge reroute's gorge), the gorge continues downstream and the carve cuts it
     out until the ground falls under it - no step at the unit edge, no water climbing. At most 2 x "Unit breach
     depth" lower (deeper carves a canyon for kilometres).
   * An UNBACKED inlet (the upstream unit present, but none of its rivers ends at that point) GROWS IN: width and depth
     from 0 over its first 200 px (`c_growInPx`), the surface kept. An UNDERSIZED one (the arriving river under half
     its width) grows from that width. An OVERSIZED one (the arriving river wider) is WIDENED to the arriving q /
     half-width / depth, and so are the segments it flows on into.
   * Every inlet starts a channel along its drain path, even under "Channel min Q"; such a segment is `needsUpstream`
     and is dropped (count 0) unless an upstream river arrives there.
2. **THE OPEN ENDS** (`fadeOpenEnds`): a segment ending neither in water nor in another river - `Edge`, `Sink`, `Dry`
   - FADES OUT the same way over its last 200 px (at most half the segment), where no end lake formed. **An UNMATCHED
   OUTLET fades too**: its river reaches the crossing more than 2 x "Unit breach depth" under the crossing's level
   (past what the inlet match lowers), so the neighbour's inlet counts as UNBACKED and grows in from nothing.
3. **The piece grid**: a 16 px grid over the unit grown by the reach, listing the pieces (point k -> k+1) whose reach
   touches each cell - so a query also finds a NEIGHBOUR unit's river near the edge - and per-row lake-run offsets.
   **A cell lists its pieces segment by segment** (the carve relies on it). Also the SECTIONS (`pointSection`, below).
   Built after 1 and 2, so the grid sees the widths.
4. **SUNK UNDER THE OTHER RIVERS' CARVES** (`sinkUnderCarves`): the profile walked the UNCARVED ground, but another
   river's valley wall cuts the hillside down to "Valley slope", so a steep stream on a big river's valley side stands
   above the carved ground. Each point's surface is held at or below the ground the other segments' carves leave there
   (never below that river's own surface), non-rising downstream, two passes; the rapids / fall marks are re-derived.
   Only the unit's own rivers: a neighbour unit's wall across the boundary is not seen.
5. **THE LAKE LEVELS** (`meetLakeLevels`; **after** `sinkUnderCarves`, whose lake-bed carves would sink the outlet's head
   under the lake again): a lake is drawn at its level, a river at `surface()` (its water less the hydraulic depth and
   the valley sink), so a river leaving a lake at the level stands that much under the lake's water. A segment starting
   in a lake keeps its surface at the level (less 0.2 model m: no z-fight) to its last point in the lake, then eases
   down to each point's OWN surface over 16 px of DISTANCE FROM THE LAKE (not along the river: one leaving along the
   shore stands under the lake behind a thin bank), held non-rising by a running minimum, carried on through junctions
   until done. One ending in a lake has its surface floored at the level, and the segments flowing into it are floored
   at its start, through junctions. Only raises.
6. **THE PIECE CONSTANTS** (`PreparedRiverUnit::pieces`, `pointSurface`), from the final points: per piece its surface
   drop, its steepness (`pieceSteep`) and an upper bound of its reach over the whole piece. `apply` skips a piece past that
   bound (and past its humidity spread) before any `pow`, and calls `wallReach` once per piece and point.
   `sinkUnderCarves` cannot use them: it moves the water while it carves, so it calls `pieceSteep` itself.

**The carve** (model frame, per piece, d = distance from the centre line; the unit's W, half-width w, depth and Q
interpolated).

* **The water surface S = the unit's W minus its depth** (the profile puts W a hydraulic depth ABOVE the ground under
  it) **minus the valley incision** "Valley depth (m)" + "Valley depth per Q" x Q^0.4 (`RiverCarveConfig::surface`).
  The channel depth D = max(depth x "Channel depth scale", "Channel min depth (m)").
* **Cross-section** (`RiverCarveConfig::channelCut`): the SIDES at "Channel wall slope" (rise / run) from S at d = w
  down to a FLAT bed at S - D, rounded at the foot (a smooth min over 0.15 D). The depth does not steepen the sides; a
  deeper channel's are only longer, and where the sides would not reach D within 80 % of the half-width they steepen
  just enough to. Then ONE CURVE over the bank and the floodplain (w x ("Bank factor" + "Floodplain factor") wide)
  rising "Bank height" x the HYDRAULIC depth (not D: "Channel depth scale" deepens the channel under the water only) as
  (x / width)^"Floodplain curve" (1 straight, 2 a bowl), then the valley wall at "Valley slope".
* **The floodplain is always carved whole**: "Carve reach (m)" is how far the WALL may run past the floodplain's edge
  (fading out over its last 30 %), **scaled with the water**: the whole reach from "Carve reach Q" up,
  (Q / it)^"Carve reach Q exponent" of it below, x the channel's growth - a small stream cuts a small valley, a head
  none (`RiverTerrain::wallReach`); a piece's reach = its floodplain edge (capped at 1500 model m, the lookups' bounded
  margin) + that. All of these are carve-time only (a maps rebuild, no unit rebuild). Near the coast the incision can
  sink the valley floor below sea level, and the sea then floods it (an estuary).
* **It only ever lowers** (soft min with the ground), the wall fading out as above (`pieceCarve`).
* **THE WHITEWATER CHANNEL**: on rapids and falls `pieceCarve` deepens the channel up to "Whitewater channel depth" x
  and widens it up to "Whitewater channel widen" x by the piece's steepness (its own drop eased from half "Rapids slope"
  to "Fall slope", a plunge gorge in full), so the falling sheet and the rapids' waves do not dip into the bed. The
  widened part is a SHELF just under the waterline (5 % of the hydraulic depth x the steepness), not channel; cut at the
  channel's slope the widening lies below the water and the surface stands above the banks on every slope.
* **Plunge gorges** (`RiverPoint_Gorge`, interpolated along the piece): past the channel's edge the walls rise at
  "Gorge wall slope" (default 2) with no floodplain, fading back to the valley profile at the gorge's end.
* **A SEGMENT carves with the 1 / d^6 blend of its pieces' carves** (`segmentWeight`: its own piece alone on the river
  line, a continuous ramp where two parts of one river meet), and the segments compose by min. **A min over every piece
  lets a reach steeper than "Valley slope" cut under its OWN upstream water; the nearest piece alone leaves cliffs where
  it switches.** Every piece's blend weight fades to 0 over the last 30 % of its wall reach (cut off at full weight,
  plunge-gorge walls show a comb of steps).
* **A waterfall's lip splits a segment into SECTIONS** (`pointSection`, from the `RiverPoint_Lip` flag `shapeFalls`
  sets): a point blends only the pieces of its NEAREST piece's section, so the ground breaks at the lip instead of a
  rounded ramp. **Only near the channel**: fully within the nearest piece's floodplain edge, fading (smoothstep) to the
  blend of every piece at twice it. Where the section has no carve, the blend of every piece stands (never the raw
  ground). That wide blend nearly leaves the plunge gorges out (weight 0.1 %: their water stands the whole drop lower),
  keeping the hand-over continuous.
* **UNDER THE DROP**: a point whose nearest piece is the lip's own piece takes the section BELOW it, so the step stands
  at the lip's top; the 0.6 px lip run is more than the terrain mesh's vertex spacing, so the mesh's face from that top
  is steeper than the sheet and lies behind it. The lip's OWN piece keeps 0.001 of its blend weight (it falls the whole
  drop over 0.6 px). **THE DROP'S FACE RAMPS** (a hard switch draws an oblique cliff as a comb of spikes): below the lip
  the ground goes from the section above to its own along the river, measured from the lip's line as
  sqrt(dAbove^2 - dBelow^2). Within "Fall clearance" x the channel's half-width the face under the sheet is a
  smoothstep to the lip piece's foot that starts BEFORE the lip, as far as "Fall face slope" (rise / run) needs; there
  the ground is also held a channel depth under the sheet of THIS drop's lip, and before the lip under a line rising
  upstream at the face slope. From the clearance out to the floodplain edge the face grows to the length that keeps it
  at about "Gorge wall slope" (1.5 x drop / slope). The fade to the wide blend runs over max(floodplain edge,
  1.5 x |section - wide| / "Gorge wall slope"). The face's top and bottom edges still show some teeth (accepted).
* **A pure function of (x, z), never of the grid step** - the terrain's edge stitch interpolates the finer node's own
  samples, so a step-dependent carve would crack. Inside the channel the carve replaces the ground, so the crag detail
  is gone there without a mask.

**Water** (`apply`):

* Inside a perennial channel `waterKind = River`, `inlandWater` = S (world; the debug lines draw S too),
  `flowAngle01` = the piece's direction; an ephemeral channel or a pan sets `dryBed`; a lake's wet pixel (nearest
  native pixel) sets `waterKind = Lake` + its level where above the ground. `riverSpeed` = the most influential piece's
  Manning speed (as `segmentWater`; 0 on a dry bed). `river` = 1 in the channel fading to 0 at the floodplain's edge,
  and 1 over a lake's wet pixels fading to 0 within "Lake shore (px)" (the beach on the bed and the shoreline, no
  grass / clutter under the water).
* **`waterLevel` stays the sea's** (`TerrainPoint`, `ETerrainWater`), so the ocean, its swash, its buoyancy and the
  bake's water channel never see inland water - no gate in the bake. Coarse queries pass straight through.
* **The lake bed** (on that pixel's ground, before the level test): a carved end lake's dug to 3 model m under its
  level; then any lake's lowered by "Lake bed deepen (m)", growing in from the shore over "Lake bed deepen reach (px)"
  as a SMOOTHSTEP - flat at the waterline, so the shore keeps its own slope (the shore = the nearest pixel no wet run
  covers; touching runs of two lakes are one water, a pan is shore, past the unit is not).
* **THE RIM GUARD**: the carve knows nothing of lakes - an outlet's floodplain and valley wall would cut the rim away and
  leave the lake's sheet in the air. Beside a lake (within 3 px; not on its wet pixels, not inside a river's channel,
  which must cut through) the ground is kept at the nearest lake's level + 0.1 model m at the shore pixel, that floor
  falling at "Valley slope" farther out, never above the uncarved ground (only the unit the point lies in is searched).
* **THE WATER HUMIDITY** (a riparian boost): `apply` pulls `TerrainPoint::humidity` toward 1 by "Water humidity" x a
  factor that is 1 at a perennial river's channel edge (x smoothstep(Q / "Water humidity full Q")) or a lake's shore,
  fading (smoothstep) to 0 over "Water humidity spread (m)" (engine m) - the max over the rivers and lakes near the
  point. The sampled climate only: the units are routed from the raw tile planes, so it never feeds back into the
  rivers. Everything reading the wrapped sampler at Full detail sees it (the data map's near cascade, trees, rocks,
  clutter); the far cascade samples Coarse (no rivers), so it fades out where that takes over. A river reaches only as
  far as its pieces are listed in the cell (its carve reach); the lake search runs out to the spread.

**Consumers.**

* **The terrain mesh carries the river to the GPU**: `generateChunk` writes `TerrainPoint::river` into the vertex's u
  and 1 (inland water / dry bed) into its v - not a texture coordinate. The splat paints the bed with the beach layer
  and the grass / clutter culls read the same value (RendererVK "Terrain and ocean integration"). Its resolution is the
  drawn node's vertex spacing (2 m at LOD0).
* TreeWorld's water term (trees AND rocks) takes `max(sea, inlandWater)` where there is inland water, so nothing places
  in a lake. **A river's bed is kept clear by its INFLUENCE instead** ("Vegetation clear": no tree or rock where
  `river` is above it), read from its OWN 1 m grid (`ITerrainSampler::sampleRiverGrid`: the influence alone, from the
  river units, no terrain - cheap), the MAX of the cell's corners. At mpp 5 most beds are 1-3 m wide: the coarser
  terrain grid's points miss them.
