# Rivers

> Part of the Procedural documentation ([`Docs/Context/Procedural/CONTEXT.md`](CONTEXT.md)) — read that first.

### Rivers: the coarse network (`:RiverNetwork`)

`Private/Rivers/` (V0 built 2026-10-08: the coarse level, drawn on the preview
only - nothing in the sampler yet). **One coarse pixel IS one full tile** (256 native px each), so a coarse flow
direction is the direction water leaves a full tile.

`CoarseRiverNetwork(generator, RiverConfig)` → `tile(ti, tj)`: per coarse tile a `CoarseRiverTile` (D4 receiver,
outflow Q in m3/s, catchment in coarse pixels, filled height, `ERiverWater` Land / Sea / Lake / TerminalLake / Pan,
lake level). **Each tile is routed over its OWN domain** (the `(2R+1)^2` coarse tiles centred on it, "Coarse domain"),
so the result never depends on the order tiles were asked for; neighbours can disagree only on rivers whose catchment
is bigger than the margin. Steps (`RiverNetwork.cpp`):

* **Sea** = deeper than "Sea depth", plus below-zero ground connected to it - V3's -0.05 m film is land.
* **Priority-flood** (Barnes, with the FIFO pit queue for flats) from the coast and the domain edge: a D4 receiver and
  a filled height per pixel; the pop order is downstream-first.
* **Runoff** = rain - Budyko (Fu) evaporation, from the tile planes (`fetchCoarseTilePlanes`: raw precip mm/yr,
  tempSea + the config's lapse and temperature offset, the humidity offset as a precip offset - the climate the
  biomes show).
* **Depressions** deeper than "Breach depth" and at least "Lake min cells" are lakes - and so is ANY depression deeper
  than 2 x "Breach depth", however small (a pond; the same rule in the units with their "Unit" values: cut through, a
  deep small basin left a river a gorge tens of metres deep, which floated or canyoned across the unit edge,
  2026-10-09); the rest pass water through
  (no carve yet). **THE SIZE LIMIT** (units only, "Unit lake max cells", default 20000, 0 = none; the user, 2026-10-09:
  a deep valley basin filled to its spill was a 292000 px lake): the level goes between the basin's LOWEST that many
  pixels and the next (a terminal lake's hypsometry), and of the pools under it the LARGEST is the lake (the one
  around the lowest pixel was a thin strip in the outflow's gorge); the other pools and everything above are land
  again - the water runs over them as channels, and the outflow's breach profile cuts down through the rim to the lower
  level (a gorge, at most the basin's depth). **The basin's drain tree is rebuilt** (`capDepression`): the flood's
  tree ran from the spill through the whole basin, so its paths left the smaller lake over a dry stretch and came back,
  or ran from a pool into the lake - rivers that started and ended in the same lake. Now ONE path leaves (the old one
  from the lake's lowest pixel, from where it last leaves the lake) and every other basin pixel drains into the lake or
  onto it: a priority flood from the path, lowest first, so the whole lake fills from its exit before any dry pixel. The
  basin keeps its rank slots, reassigned in the new tree's order. **THE EDGE CAP** (units, `edgeHoldsNoLake`): the
  neighbour unit cannot see a unit's lake, so a lake over or beside a grid-edge pixel (the unit's edge or a missing
  tile's; near an inlet no soft wall holds the border) stood in the air where the neighbour's ground fell away - a lake
  on a clifftop at a unit edge (2026-10-09). Its level stays 0.1 model m under the lowest such pixel; only the pixels
  under that stay lake, through the same `capDepression` (and none left = no lake, the river runs through the basin). A lake gathers its inflow and releases at its exits when the last one is in: full (spills inflow +
  own rain - open-water evaporation), terminal (the lowest cells that evaporate exactly the inflow, no outflow) or a
  salt pan (under one cell). Dry land loses channel water by aridity (`loss x (PET/P - 1) x km x sqrt(Q)`).

Not disk-cached: the input coarse tiles are, and a domain routes in milliseconds. The planes are cached per network
object. **`TerrainPreview` draws it** (rivers by Q, lakes, brackish terminal lakes, pans, dry beds), and a
"Terrain/Rivers" change re-colours the published map from its stored samples, with the map kept up.

**`:RiverRouting` is the router both levels run** (`routeDrainage` over a `DrainageGrid`: optional mask, D4 or D8,
caller seeds, inlet injections; `markSea`; `riverPixelClimate`). A masked region no seed reaches drains to its own
lowest pixel. The coarse level runs it D4 (a coarse link is a tile edge); the units D8.

### Rivers: the units (`:RiverUnits`, `:RiverSystem`)

V1 built 2026-10-08, carved into the sampler since V2 (below). **A unit = N x N full tiles** ("Unit
tiles", default 8) on a fixed model-space lattice, routed at native resolution by `buildRiverUnit`
(`RiverUnits.cpp`):

* **Crossings** = the coarse links across the unit's edge (or into one of its missing tiles). The point is the low
  point of `min(this tile's border, the neighbour's border)` (box-smoothed, inside "Crossing window" of the edge's
  middle) - **both units read the same two tiles, so they pick the same point**. Q = the coarse link's, water level
  = bed + depth(Q). Outlets are flood seeds, inlets inject their Q. A tile whose coarse water ends in it (dir None)
  with no native sea drains to its lowest pixel.
* **The two sides of a crossing MEET**: the inlet's segment starts and the outlet's segment ends on the shared point
  on the tile boundary between the two border pixels (both units compute it), with the crossing's Q; the outlet side
  blends its own fine Q into the crossing's over its last 24 px (width, depth and valley follow Q) and ends at or
  below the crossing's level. Before, the river stopped a pixel short on each side and changed width and height at
  the unit edge.
* **Every other edge pixel is a SOFT wall**: a seed ordered "Edge wall" model m above its ground
  (`DrainageSeed::priority`), so water leaves there only when every way to an outlet climbs more, and its segment ends
  `ERiverEnd::Edge` (the neighbour does not continue it). **A basin that spills through a soft seed is never a lake**
  (`DrainageResult::root`). With hard walls, valleys whose fine watershed crossed a unit edge filled up to the lowest
  pass toward an outlet: lakes far above the hillside, cut off at the unit edge (seen 2026-10-08). **No soft wall
  within 32 px of an inlet** (`c_inletGuardPx`): the border pixels beside it were soft seeds too, and where the ground
  just inside rose more than the wall the arriving river ran straight back out beside where it came in - one pixel
  long, then faded to nothing at the edge (2026-10-09).
* **A crossing's DIRECTION** (2026-10-09): both units end a crossing segment on the same boundary point, but each came
  in at its own angle and the channel kinked. Both agree on the edge's NORMAL: after the path shaping (Chaikin), the
  stretch within 16 px of the crossing is pulled onto the line through the boundary point along it - fully at the point,
  fading out with the distance (smoothstep) - so the two sides meet in one direction.
* **THE EDGE REROUTE** (2026-10-09, the user's pick): the coarse network decides where water crosses an edge, the fine
  routing where it goes - over a ridge higher than "Edge wall" a ~20 m3/s river left through the soft wall (ending at the
  border) while the coarse crossing 500 px along the same edge got nothing, and the neighbour, which injects the coarse
  Q there, started a full-size river from nothing. So after `routeDrainage` an OUTLET whose fine water falls under half
  its coarse Q takes the largest SOFT EXIT on the same unit edge within 3 tiles carrying at least 30 % of that Q: a
  least-climb Dijkstra inside the unit (within 48 px of the edge, off the border pixels, not through the sea or a lake;
  cost = the step x (1 + 0.5 per model m above the exit's ground)) carries the exit's water on to the outlet - the
  receivers and outflows along it rewritten, its pixels given the exit's rank (whatever joins them is profiled first).
  The outlet's Q blend matches the coarse Q. Each exit is used once. **Not past a rim of 2 x "Unit breach depth"**:
  routing never cuts deeper (a deeper basin is a pond, below), so there the fine and coarse drainage really disagree. The breach keeps the exit's level through the rim
  (a gorge - 23 m in the case seen), so the water reaches the crossing LOWER than the crossing's own level (the edge
  ground + depth), where the neighbour starts: the sampler's INLET MATCH (below) carries it on.
* **THE END LAKES** (2026-10-09, the user: rivers should end in the sea or a lake): a perennial segment ending in a
  `Sink`, running `Dry`, or reaching a soft wall floods a TERMINAL LAKE at its end. An `Edge` end is first pulled back
  8 px from the border (`c_edgeBackPx`) and becomes `Dry`: the neighbour does not see it (seen: a big river ran into
  the border of a flat valley, the neighbour's stream into the same point from the other side). The flood starts at the
  last pixel and takes the LOWEST GROUND FIRST, so the lake has the terrain's own shape (up the river's own valley,
  into side hollows). It grows to "End lake size" px per m3/s arriving (at most 20000 px). It never goes into the edge
  band (2 px), the sea or another lake; those are only its shore. **HELD**: the flood rose over a rim with every shore
  pixel above its level. The lake fills to there, and a held size of at least half the target counts. **Else CARVED**
  (the user's pick: the flood ran down a slope or out into the edge band / the sea). Its SHAPE comes from a second
  flood: lowest ground alone gave the pixels under one contour - on an even slope a straight shore that moved with "End
  lake size" - so the cost is the ground + 1 model m per px from a CENTRE + an 8 model m value noise over 8 px cells (in
  global pixels): a compact, uneven lake, still along the low ground where the ground has a shape. The centre is the
  end moved off the unit's edges by 1.5 x the round radius + 12 px: from an Edge end (8 px in) the blob grew round the
  end and the edge band cut half of it off straight. Its pixels sit at 0.1 model m under their lowest shore pixel, `RiverLake::carved`, and RiverTerrain digs their ground to 3 model m under the level
  (`c_carvedLakeBedM`). **Never above the arriving river's water**: the breach profile can run the river in a gorge
  far under the ground the flood saw (a lake at that ground stood 138 model m over the river, 2026-10-09), so a level
  over the end's water drops to it, carved. Not when the digging is deeper than "End lake max depth" under the highest
  pixel (then the river fades out). **Another river's
  channel ENDS the flood** (only channel pixels that drain to this end, or its own cut-off tail, join; the lake is then
  held or carved at what it has): covered, that river ended in the lake and the river after it started from nothing
  past the shore; only skipped, the lake grew round it on both sides and the river ran through the lake on a strip of
  shore. At least 16 px, or
  no lake (the fade-out stays). **Every segment ends where it first enters an end lake** (end `Lake`; sampled along
  its pieces every half pixel and the end point moved there - the simplified points are sparse, and a straight reach
  crossed a whole lake with no point inside it; its water stays its own; a segment that starts in one is gone): the valley-first flood covers the river's last
  stretch, whose surface stood over the lake's in near-cell rectangles. **A lake cannot cross a unit edge**: two units
  that both end rivers at the same border point make two lakes, with a ground strip and two levels between them.
* **THE LAKE CHANNELS** (the user, 2026-10-10): a chain stopped at a lake's FIRST pixel, and the drawn ground (the
  tiles' detail over the routing's raw pixels) stood a little above the level at the shore - land with trees between
  the river's end and the open water. A `Lake` end now runs on into the SAME lake down the steepest way (8 neighbours,
  never back) until the raw ground is "Lake channel depth" model m under the level (default 12) or "Lake channel max
  length" px (40), at the lake's level (the segment's floor) and the arriving Q: the carve cuts through the shore into
  the bed. **The same for a river LEAVING a lake**: when a segment's start takes a lake pixel's water (a donor in a
  lake), the chain runs BACK into that lake the same way, prepended (`lakeHead`), at the lake's level with the start's Q,
  and the river falls from there - the outlet is cut through the shore. The ribbon on those stretches lies under the
  lake's surface (the carve's surface sinks below the unit level).
* **THE COAST the coarse network sees** (2026-10-10): in a tile whose COARSE pixel is sea, every pixel under 0 is sea
  too. `markSea` seeds only below "Sea depth" (V3's sea-level film is not sea), so a shallow coastal tile had no sea at
  all: the coarse river ended in it, the fine one found no water there.
* **THE SEA RESCUE** (the user: rivers should end in the sea or a lake, carving if they must): a soft exit still
  carrying at least "Sea rescue min Q" (5 m3/s) - a river leaving where the coarse network has no crossing, which the
  neighbour never carries on - is sent along the LOWEST-RIM path inside the unit to a sea or lake pixel (minimax: the key
  is the highest ground on the way in whole model m x 1e4 + the length; never along the border or off the present
  tiles), when that rim is at most "Sea rescue max rim" (150 model m) over the exit's ground: the breach profile cuts
  it, and the river ends in the sea (its sea channel) or the lake. **The direction comes from the coarse network**:
  from the exit's tile its flow directions are followed tile by tile to a coarse Sea / Lake / TerminalLake tile in the
  unit, and the fine search keeps to that route's tiles and their 8 neighbours (a CORRIDOR); a route that leaves the
  unit or ends in a dry coarse sink means no rescue (the coarse network sends the water elsewhere). Paths longer than
  "Sea rescue max length" (1500 px) are not followed. **From the river's MAIN STEM, not its exit**: started at the
  exit, the river ran on to the border and turned back along the path - a wide dead end at the edge. Every pixel
  upstream from the exit along the largest inflow (while it carries at least half the exit's water, to the max length)
  is a start; the key is the highest ABSOLUTE ground on the way (whole model m + an offset, x 1e4, + the length, in
  doubles), so the branch is wherever the lowest ridge is reached soonest. The rim is checked against the branch's own
  ground ("Sea rescue max rim", 300 model m), and the stem below the branch loses the branch's water (it falls under
  "Channel min Q": no dead end). **The path wanders**: past the ridge every way had the same key and the plain shortest
  one won - long straight D8 runs, and over a cut's steep ground the meander stays small - so a step's length costs x
  (1 + 1.5 x a value noise over 24 px cells in global pixels); the true length still caps the path at the max length. Receivers / outflows / ranks rewritten as the edge
  reroute. Seen at (9,-20): a 224 m3/s river entered from the east and left through the same edge's soft wall - the
  lowest way to its sea climbed 264 model m (the coarse network breaches it at 1.28 km a pixel), and the neighbour's
  stream drains to the same border point: a basin across the unit edge (a cross-edge lake, not built).
* **THE SEA CHANNEL** (the user, 2026-10-10): a chain stopped at the FIRST sea pixel, and on a wide coastal flat at about
  0 the river met an ocean almost 0 deep there and seemed to stop. A `Sea` end now runs on into the sea down the
  steepest way (8 neighbours, sea pixels only, never back) until the floor is "Sea channel depth" model m deep (default
  60, ~10 engine m) or "Sea channel max length" px; its Q is the mouth's, its level falls from 0 at the mouth to minus
  that depth at the end (the ground clamp's floor follows), so the carve cuts a channel out to deep water and the ocean
  fills it. RiverSystem's ribbon stops where the level drops under -0.5 model m (`c_seaChannelCut`): the ocean is the
  water there; `apply` does not mark such channel points River (they stay sea below sea level).
  **An inlet on a sea pixel starts a segment too** (`Sea` at once, with the inlet's Q, then the sea channel): the sea is
  marked per unit, so on a flat just under 0 one crossing can be land upstream and sea downstream - the river ran to
  the unit edge and stopped.
* **Each tile's land runoff is rescaled to its coarse pixel's own** (`CoarseRiverTile::runoff`), so the levels agree on
  the budget.
* **Channels** (Q >= "Channel min Q", outside lake basins) become segments from heads / junctions / inlets to a
  junction, an outlet, the sea, a lake basin's rim, a sink or dry (losses took the water). Segments are profiled
  **UPSTREAM first with pure breach semantics**: a segment starts at the lowest of its own bed + depth, the water its
  tributaries bring and (an inlet) its crossing's level, then W = min(W above, bed + depth) - it never rises, so a rim
  in its way is CUT (the carve's gorge), never filled. Only the sea (0) and a lake (its level) floor it. (The first
  version floored each segment at the level its downstream segment STARTED at - in a filled basin that is the rim, so
  whole rivers stood at rim height over the basin floor. An outlet crossing's level no longer floors the upstream side
  either, so the two units can meet at slightly different levels at a crossing.) Then the PATH SMOOTHING: the D8 path (straight runs at 0 / 45 / 90 degrees with sharp kinks - the first
  version only rounded each kink inside a pixel and the rivers looked angular) is resampled at 1 px of arc length and
  Gaussian-averaged along its length ("Path smoothing (px)", sigma, default 10 = 50 world m at mpp 5); the window shrinks
  symmetrically toward the ends, so junctions and crossings stay put. Then the MEANDERS (`meander`; the user: "rivers
  are basically never straight for long"): a sideways swing, two sines over an ACCUMULATED phase - wavelength "Meander
  wavelength" x the channel width (it follows the width as it changes; at least 4 px, the width at least 0.2 px), swing
  "Meander amplitude" x the width - SMALL STREAMS wind tighter and wider in widths: x "Meander small wavelength" / x
  "Meander small amplitude" at "Channel min Q", blending (log Q) to x 1 at "Meander full Q" - phase from
  the segment's start in global pixels - faded to 0 within half a wavelength of each end, shrunk toward 30 % as the water
  steepens past "Meander slope", and KEPT ON THE VALLEY FLOOR: the largest of 1, 3/4, 1/2, 1/4 of the swing whose
  ground stands at most the channel's depth above the point's own (else none). The steepness factor and that pull-back
  are both BOX-SMOOTHED over half a meander wavelength (8..64 px), with no cap: per 1 px sample they jumped (the breach
  profile's staircase of flats and drops flips the steepness 1 / 0.3) and each jump moved the swing sideways at once -
  Z-shaped jogs (2026-10-09). Then
  Chaikin x2, then THE GROUND CLAMP (`clampToGround`): W was walked along the raw D8 pixels and the shaping moved the
  path with W tied to its arc position, so where the final path lies over LOWER ground W stood in the air (seen
  2026-10-09; the carve only lowers). On the final path W <= the ground under it + the channel's depth, then a running
  min downstream again - **never under the profile's floor** (a `Lake` end's lake level, a `Sea` end's 0): over the
  lake's bed the clamp pulled a river 38 model m under its lake's surface, and the lake's sheet stood over it. **THE
  WATERFALLS** (`shapeFalls`, V6, on the FINAL points after the simplify - the shaping would spread a step again): a run
  of pieces steeper than "Fall slope", at most "Fall max length" px x clamp(Q at the lip / "Fall full Q") - the water
  cuts the gorge, a small stream none (its steep run must be all but a step) - dropping at least "Fall min height" model m becomes
  a straight drop at its TOP - the water keeps its level to the lip, drops over 0.6 px (a second point) to the run's
  lowest level and runs on at it to the run's end (the lip's top point carries `RiverPoint_Lip`; the foot and the run's
  points but its last `RiverPoint_Gorge`), so the carve cuts a plunge gorge back into the slope - a NARROW one: in
  `pieceCarve`, the gorge mark interpolated along the piece, past the channel's edge the walls rise at "Gorge wall
  slope" (default 0.75) with no floodplain, fading back to the valley profile at the gorge's end (the valley carve cut a
  trench far wider than the river down to the lowered water). **THE WHITEWATER CHANNEL** (the user: the terrain makes
  room, the water stays): on rapids and falls `pieceCarve` deepens the channel up to "Whitewater channel depth" x and
  widens it up to "Whitewater channel widen" x (defaults 3 and 2; the ribbon is 1.2 x the channel, so past 1.2 the channel holds its edges) by the piece's
  steepness - its own drop eased from half "Rapids slope" to "Fall slope", a plunge gorge in full - else the falling
  sheet and the rapids' waves dipped into the bed and the banks' tips poked through the sheet's edges. The widened part
  is a SHELF just under the waterline (5 % of the hydraulic depth x the steepness), not channel: the deep cut keeps the
  river's own half-width, the bank starts past the shelf - cut at the channel's slope the widening lay below the water,
  the ribbon's edge hung over it and the surface stood above the banks on every slope (at the bottom
  the water would stand over the falling ground above it). A longer steep run stays sloped rapids - WHOLE (stepping one
  point on, a long run's tail came under the max length and fell halfway down the slope: its gorge cut into the hillside,
  the fall buried in it, the water above standing over the gorge's wall). RiverTerrain's
  carve does not blend across a lip (SECTIONS, below). Then Douglas-Peucker that keeps a
  point when EITHER its plan offset passes 0.1 px OR its W passes 0.25 model m from the interpolation - W is linear
  between the kept points, and the plan-only version kept just the ends of a reach that was straight in plan but
  dropped into a canyon partway, so the water sloped through the air over the drop. Points carry W, half-width, depth, Q and rapids / fall flags (by
  the W slope); a segment never above "Perennial Q" is ephemeral.
* **Lakes** are stored as wet-pixel row runs plus a level and kind per lake.
* **Disk cache** `Local/Diffusion/<seed>/river_x<j>_z<i>_n<N>.rvu`, keyed by a hash of every river setting and the
  generator's climate shaping. **Only a unit whose tiles and edge neighbours all lie inside the generated bounds is
  cached** - past them the result depends on which tiles exist.

`RiverSystem` (the WATER and the debug view) is a TerrainStreamer MEMBER (no global): `rebuildMaps` hands it the live
`RiverTerrain`, `update` runs after `updateTerrainTextures` (after beginFrame: it pushes nodes). One Low job pulls the
nearest missing unit inside max("Surface/Radius" while the water is on, "Debug radius" while the lines are) from the
sampler's store (built there if not resident); units past 1.5 x the radius go. **The water** (V3, "Terrain/Rivers/
Surface"): the pull job also builds the unit's water meshes (`buildSurfaceMeshes`, pure): one WATER TILE per terrain
BASE CHUNK it has water in ("Chunk size"; a ribbon quad goes to its midpoint's chunk, a lake run is cut at the chunk
columns) - a RIBBON per perennial segment at
the carved water surface (`RiverCarveConfig::surface`), 1.2 x the channel's half-width (the carved bank hides the rest),
with the Manning-like flow speed (capped at 6 m/s; on a fall-steep piece blended toward 0.7 x the free-fall speed sqrt(2 g h)
over its drop, up to 9 m/s - the sheet crept down; each point then raised toward any faster water downstream, fading
over 30 m - the calm reach above a fall crept to its lip - then smoothed along the river by DISTANCE, only ever raising
(averaged, a fall's few metres of sheet took the calm water's speed), a Gaussian of sigma 20 m: per point it jumped
from a plunge pool's still water to the next reach's full speed between two far-apart points, and the flow texture
stretched across the change) and the WHITEWATER per vertex - a smooth measure of how steep the carved water runs
(0 below half "Rapids slope", 1 at "Fall slope", eased; blurred along the river - `segmentWater`), not the rapids / fall
flags, which only colour the debug lines. **At a JUNCTION** (`segmentWater`): the river flowed into stands lower (its
bigger Q: a deeper channel and valley sink), so a segment ending there EASES DOWN to that river's surface over its last
3 x the main half-width (at least 20 m) - and its FLOW (direction and Manning speed) and WHITEWATER blend into that
river's first piece's over the same stretch (a slow stream joining a fast river showed a sheet of slow water) - and a clearly smaller one (under 0.6 x the main half-width: a tributary) STOPS
0.85 x the main half-width before the junction, just inside the main water, instead of lying over it as a second sheet;
the LARGEST arriving branch is only eased, never cut (it is the main river's continuation; with both of two similar
streams cut, the junction's upstream side had no water). A QUAD per lake row run at the lake's level,
one pixel wider all round (the ground cuts the shoreline; pans have none). Main uploads each (`createMesh`, no BLAS) and
spawns its node at the unit's origin at sea level, `PASS_MAIN` (0 while the tile is past "Radius"). **IT RIDES THE
TERRAIN'S CULLING** (2026-10-12: one ~7.7 km mesh per unit was never culled): no spatial entry of its own - the tiles
are listed per base chunk (`m_tilesByChunk`, a near cell under the chunk below its centre too), TerrainStreamer's
hand-over walk lists every chunk it drew in the main pass (live: entry valid; `firstBase` + span 2^lod) and at its end
calls `RiverSystem::drawChunks` ON ITS WORKER, which pushes the tiles over them (once each: a LOD handover's two
residents). Water is drawn exactly where its ground is. The tiles only change in `update`, which TerrainStreamer calls
after joining the walk, so nothing is retired. With culling off `update` pushes every tile itself. A "Chunk size"
change re-pulls the units. Drawn by RendererVK's
River variant (RendererVK CONTEXT "River and lake water"). Toggling "Surface/Enabled" re-pulls the units.
**The near cells** (the waves' geometry): 64 m squares (`c_nearCell`, = the shader's `RIVER_NEAR_CELL`) whose centre is
within "Near radius" + one cell, each ONE dense mesh of every perennial river crossing it (`buildNearCellMesh`: each
segment's water - `segmentWater`, shared with the light ribbons so the two lie on one another - resampled every "Near
spacing" m, "Near across" vertices wide; a row pair belongs to the cell its first row lies in), built nearest first on
their own Low job from the resident units near the cell, one node per cell at its origin, `PASS_MAIN`, drawn with its chunk as a tile. A cell also
holds a dense LAKE grid: every `spacing` m a vertex where a wet lake pixel lies within one pixel (the flat quads' margin),
at that lake's level, a quad where all four corners are in - the VS gives it the lakes' waves. Each frame RiverSystem
reports the NEAR COVERAGE (`Renderer::setRiverNearCovered`: the nearest missing cell's centre less half its diagonal,
at most the radius); the light ribbons and flat lake quads discard inside it. A unit adopted or
evicted bumps the unit generation: every cell goes STALE and keeps its mesh until its rebuild lands; a near-mesh setting
change drops them. Lines: channels blue by Q, ephemeral tan, rapids orange, falls red, the channel's edges white and the
carved bed's outer edge (half-width x (1 + bank + floodplain)) dim green, outlet / inlet ticks magenta / green,
lake row hatching.
**The mist sources** (`updateMist`, V6): the unit pull job collects, with the water mesh, every water point with any
whitewater (`MistPoint`: surface point, channel half-width, flow, whitewater, channel depth, the stretch it stands for -
half the river to each neighbour). Each frame the resident units' points within "Mist radius" go to
`Renderer::setRiverMistSources` (nearest first past `MAX_RIVER_MIST_SOURCES`; re-sent after 10 m of camera motion or a
unit change), their whitewater x the river's size (depth / "Full size depth", weighted by "Mist size weight"). The
renderer's producer spawns `Effects/river_mist.pfx` over them (see RendererVK): Procedural cannot link Particle, so the
mist goes the ocean spray's way. **A FALL'S FOOT** mists too (2026-10-11: the whitewater stretches mist at their own,
top level over the lip's few metres, so the plunge pool had none): the water point under a drop of at least 2 m,
steeper than 45 degrees, is a plunge source (`MistPoint::plunge` = the drop) at full whitewater, its stretch "Mist
plunge" m per m of drop (default 0.1, 0 = none), centred a third of it downstream - applied in `updateMist`, so the
tweak works live. Its half-width goes up NEGATIVE: the shader then takes "Plunge mist centering / speed / kick /
height" (defaults 0.18 / 0.2 / 1 m/s / 0 m - the pool's mist rises) in place of the whitewater's four.

**The inland water map** (`updateWaterMap`): `RIVER_WATER_MAP_DIM`² texels of `RIVER_WATER_MAP_TEXEL` m around the
camera, each the inland surface Y at its centre (`RiverTerrain::sampleInlandWaterGrid`: the river's calm carved surface
inside its channel, else a lake's level; units only, no terrain; the KIND in the height's lowest mantissa bit, 1 = a
river channel, for the fog's "River underwater scale"), baked on a Low job when the camera has moved
`c_waterMapMove` from the last bake's centre or the unit generation changed, handed to `Renderer::setRiverWaterMap`.
RendererVK's underwater fog and terrain wetness read it (RendererVK CONTEXT "River and lake water"). **The buoyancy
reads it too**: RiverSystem keeps the last bake's heights (`sampleWaterHeight`, nearest texel, -FLT_MAX off the map or
on dry land; `TerrainStreamer::sampleInlandWaterHeight` / `hasInlandWater` forward it), swapped on main in `update`
after the entity pass has joined, so the workers' reads on the next pass are const. The App takes the higher of it and
the ocean's surface. No waves - the calm carved surface / the lake's level.

### Rivers: the sampler (`:RiverTerrain`)

V2 built 2026-10-08. **`RiverTerrain` is an `ITerrainSampler` that WRAPS the `TerrainGenV3`** - a wrapper, not a part
of the generator, because the units are built from the generator's tiles. `rebuildMaps` publishes it as THE maps when
"Terrain/Rivers/Enabled" (default on), so the chunks, the collider, both bakes and the tree / rock records sample the
carved terrain with no change of their own. Every river row except the preview / debug ones is a streamer
config-dirty listener (a rebuild; the units then reload from disk).

* **The unit store**: `unit(ui, uj)` builds (or loads) on first touch, BLOCKING like a tile miss; concurrent requesters
  park on the unit's `JobEvent`, one builds; FIFO-evicted past 256. `resolve` takes every unit within the carve reach
  of the query rect ONCE (sampleGrid: per unit, not per point), skipping units with no tile in the generated bounds.
  On load a unit is PREPARED: a 16 px grid over the unit grown by the reach, listing the pieces (point k -> k+1) whose
  reach touches each cell - so a query also finds a NEIGHBOUR unit's river near the edge - and per-row lake-run offsets.
  **A cell lists its pieces segment by segment** (the carve relies on it). Then **THE INLET MATCH** (`matchInlets`): a
  segment starting on the unit's border looks up the upstream unit's segment ending at the same boundary point - in its
  EDGE SUMMARY (`unitEdges`: per unit only its outlet ends - position, level, half-width, depth, q - taken from its raw unit as
  built, a deduplicated store of a few floats each; units build independently, so this never chains. Building a
  summary builds the raw unit, which waits in a small HAND-OFF (16 units) for that unit's own `unit` call, so it is
  built once; the store keeps no raw units, a prepared unit only its own modified copy) - and holds its water at or below that level, and so the segments it flows on into: where the upstream water
  arrives lower than the crossing's level (the edge reroute's gorge), the gorge continues downstream and the carve cuts
  it out until the ground falls under it - no step at the unit edge, no water climbing. At most 2 x "Unit breach depth"
  lower (no breach is deeper - a deeper basin is a pond; carried on for kilometres a deeper one carved a canyon). An
  UNBACKED inlet - the upstream unit present, but none of its rivers ends at that point (its water went to a pond or
  another edge where the coarse network did not see it) - GROWS IN: its width and depth from 0 over its first 200 px
  (1 km), the surface kept, instead of a full-size river from nothing; an UNDERSIZED one (the arriving river under half
  its width at the boundary) grows from that width. An OVERSIZED one (the arriving river wider: the upstream fine
  routing gathered water the coarse network sent elsewhere - 26 m3/s at a 0.8 m3/s link, 2026-10-10) is WIDENED to the
  arriving q / half-width / depth, and so the segments it flows on into (the summary carries q and depth too). Every
  inlet starts a channel along its drain path, even under "Channel min Q"; such a segment is `needsUpstream` and is
  dropped (count 0) unless an upstream river arrives there - before, the big river stopped at the unit edge. The match
  and the open ends run on the copy BEFORE the piece grid is built, so the grid sees the widths. Upstream, the outlet's
  Q blend (`RiverUnits`) runs only where the fine Q is within half to twice the coarse one: blended from far below, a
  small stream swelled into a big river in 24 px at the edge; from far above, a big river shrank to a creek - so the
  two sides cross at the river's real size and the inlet grows or widens to it. **THE OPEN ENDS** (`fadeOpenEnds`): a segment
  ending neither in water nor in another river - `Edge` (a soft wall; the neighbour does not continue it), `Sink`, `Dry`
  - FADES OUT the same way over its last 200 px (at most half the segment); it stopped as a full-width cut. Only where
  no end lake formed (`RiverUnits` sets the end to `Lake` for one). **An UNMATCHED OUTLET fades too**: its river
  reaches the crossing more than 2 x "Unit breach depth" under the crossing's level (the edge reroute measures its rim
  from the exit's GROUND, but the water can already run under it: a 164 model m gorge to the crossing, 2026-10-09) -
  past what the inlet match lowers, so the neighbour's inlet counts as UNBACKED and grows in from nothing; before, the
  river ended full-width in its gorge and the next one started full-size 164 m higher. Then its water is SUNK UNDER THE OTHER RIVERS'
  CARVES (`sinkUnderCarves`, on a copy - the store's unit and the disk cache stay as built): the profile walked the
  UNCARVED ground, but another river's valley wall cuts the hillside down to "Valley slope", so a steep stream on a big
  river's valley side stood tens of metres above the carved ground (2026-10-09). Each point's surface is held at or
  below the ground the other segments' carves leave there (never below that river's own surface), non-rising
  downstream, two passes; the rapids / fall marks are re-derived. Only the unit's own rivers: a neighbour unit's wall
  across the boundary is not seen.
* **The carve** (model frame, per piece, d = distance from the centre line; the unit's W, half-width w, depth and Q
  interpolated). **The water surface S is the unit's W minus its depth** (the profile puts W a hydraulic depth ABOVE the
  ground under it - carving to W left the channel centre uncut and the water floating, 2026-10-08) **minus the valley
  incision** "Valley depth" + "Valley depth per Q" x Q^0.4 (`RiverCarveConfig::surface`). The channel depth
  D = max(depth x "Channel depth scale", "Channel min depth"). Cross-section (`RiverCarveConfig::channelCut`): the
  SIDES at "Channel wall slope" (rise / run) from S at d = w down to a FLAT bed at S - D, rounded at the foot (a smooth
  min over 0.15 D) - the depth does not steepen the sides, a deeper channel's are only longer, and the slope never makes
  a channel shallower: where the sides would not reach D within 80 % of the half-width they steepen just enough to (the
  user, 2026-10-09; replaced the |d/w|^"Channel shape" curve, whose sides steepened with D), then ONE CURVE over the bank and the floodplain (w x ("Bank factor" +
  "Floodplain factor") wide) rising "Bank height" x the HYDRAULIC depth (not D: "Channel depth scale" deepens the channel
  under the water only - scaling the bank too steepened it) as (x / width)^"Floodplain curve" (1 straight, 2 a bowl), then the
  valley wall at "Valley slope". **The floodplain is always carved whole**: "Carve reach" is how far the WALL may run
  past the floodplain's edge (fading out over its last 30 %), **scaled with the water**: the whole reach from "Carve
  reach Q" up, (Q / it)^"Carve reach Q exponent" of it below (default 0.1; 0.5 = the square root; higher narrows
  the small streams' valleys faster), x the channel's growth (below) - a small stream cuts a small valley, a head
  none (`RiverTerrain::wallReach`); a piece's reach = its floodplain edge (capped at 1500 model m, the lookups' bounded
  margin) + that. **THE GROWTH** (unit build, `RiverUnits.cpp`): a channel's width and depth rise smoothly from 0 at
  "Channel min Q" to the hydraulic geometry at min Q + "Fade Q", so a stream fades in at its head and out where its
  water drains away - the ribbon, the carve and the influence all follow. Depths are MODEL m (/ 6 in the world at mpp 5); all
  of these are carve-time only (no unit rebuild, a maps rebuild). Near the coast the incision can sink the valley floor
  below sea level, and the sea then floods it (an estuary). **It only ever lowers** (soft min with the ground),
  the wall fades out as above (`pieceCarve`). **A SEGMENT carves with the 1 / d^6 blend of its pieces' carves**
  (`segmentWeight`, d model m: its own piece alone on the river line, a continuous ramp where two parts of one river
  meet), and the segments compose by min. A min over every piece let a reach steeper than "Valley slope" cut under its
  OWN upstream water (the downstream pieces' lower walls reached back up the river line); the nearest piece alone left
  cliffs where it switched. **A waterfall's lip splits a segment into SECTIONS** (`pointSection`, prepare: the `RiverPoint_Lip`
  flag `shapeFalls` sets - a guess from a short, steep piece took ordinary rapids for lips): a point blends only the
  pieces of its NEAREST piece's section - beside the channel the pieces above and below a lip stood at about the same
  distance and blended the drop into a rounded ramp metres long; now the ground breaks at the lip. **Only near the
  channel**: fully within the nearest piece's floodplain edge, fading (smoothstep) to the blend of every piece at twice
  it - across the whole gorge the break drew long straight cliffs far wider than the river. Where the section has no
  carve, the blend of every piece stands (never the raw ground). That wide blend leaves the plunge gorges out (their
  `RiverPoint_Gorge` weight, all but 0.1 %): their water stands the whole drop lower and dug the bank beside the reach above a lip.
  **Every piece's blend weight fades to 0 over the last 30 % of its wall reach** (as its carve fades to the ground): cut
  off at full weight, the blend jumped where a piece left it - on a plunge gorge's walls only the far pieces counted, and
  the walls showed a comb of steps (2026-10-10). The 0.1 % keeps the hand-over to the gorge pieces continuous where
  those far pieces fade out. **THE LAKE LEVELS** (`meetLakeLevels`, prepare, 2026-10-11): a lake is drawn at its level, a
  river at `surface()` (its water less the hydraulic depth and the valley sink), so a river leaving a lake at the level
  stood that much under the lake's water. A segment starting in a lake keeps its surface at the level (less 0.2 model m:
  no z-fight) to its last point in the lake, then eases down by that shift over 16 px of DISTANCE FROM THE LAKE (not
  along the river: one leaving along the shore stood metres under the lake beside it, behind a thin bank), held
  non-rising by a running minimum. The ease blends from the level to each point's OWN surface (toward "level less the
  exit's shift", a river leaving at about the level flew at it down a hillside), and carries on through junctions until
  done. One ending in a lake has its surface floored at the level, and the segments flowing into it are floored at its
  start, through junctions (only the last one raised left a step). Only raises. It runs
  AFTER `sinkUnderCarves`: in a lake the other rivers' carves leave the lake bed, and held under it the outlet's head
  stood under the lake again. **UNDER THE DROP**: a point whose nearest piece is the lip's own piece takes the section BELOW
  it, so the step stands at the lip's top; the lip runs 0.6 px (`shapeFalls`), more than the terrain mesh's ~2 m vertex
  spacing, so the mesh's face from that top is steeper than the sheet and lies behind it. (Carved from the lip's piece,
  the face leaned through the sheet and cut it in a V; an undercut 3 m upstream of the lip left the water running
  through the air before it fell - 2026-10-10.) **THE DROP'S FACE RAMPS** (2026-10-11): the hard switch between the
  sections stood the whole drop up as a cliff across the near zone, and the terrain mesh drew a cliff oblique to its
  grid as a comb of spikes. Below the lip the ground goes from the section above to its own along the river, measured
  from the lip's line as sqrt(dAbove^2 - dBelow^2) (the nearest distances of the two sections, the lip's piece counted
  below; x is negative above the lip line). Within **"Fall clearance"** (default 1) x the half-width of the channel the
  face under the sheet is a smoothstep to the lip piece's foot that starts BEFORE the lip, under the upper water, as far
  as **"Fall face slope"** (default 6, rise / run, peak 1.5 x) needs: the sheet falls near-vertically, and a face that
  steep (or a sharp break) drew as a comb under the water (2026-10-11). There the ground is also held a channel depth
  under the sheet of THIS drop's lip (the nearest lip piece of the section above - another fall's sheet pulled the ground
  down), and before the lip under a line rising upstream at the face slope: the upper banks stood in the sheet's sides.
  From the clearance out to the floodplain edge the face grows from the lip to the length that keeps it at about "Gorge
  wall slope" (1.5 x drop / slope). The face's top and bottom edges still show some teeth (accepted). The lip's OWN piece keeps 0.001
  of its blend weight: its water falls the whole drop over the 0.6 px run, and its carve stood that drop up sideways to
  its full reach. The fade to the wide blend runs over max(floodplain edge, 1.5 x |section - wide| / "Gorge wall
  slope"), so it never stands as a step either. **A pure function of (x, z), never of the
  grid step** - the terrain's edge stitch interpolates the finer node's own samples, so a step-dependent carve would
  crack. Inside the channel the carve replaces the ground, so the crag detail is gone there without a mask.
* **Water**: inside a perennial channel `waterKind = River`, `inlandWater` = S (world; the debug lines draw S too), `flowAngle01` = the piece's
  direction; an ephemeral channel or a pan sets `dryBed`; a lake's wet pixel (nearest native pixel) sets
  `waterKind = Lake` + its level where above the ground. **The lake bed** (on that pixel's ground, before the level
  test): a carved end lake's dug to 3 model m under its level; then any lake's lowered by "Lake bed deepen (m)",
  growing in from the shore over "Lake bed deepen reach (px)" as a SMOOTHSTEP - flat at the waterline, so the shore
  keeps its own slope (the shore = the nearest pixel no wet run covers, touching runs of two lakes are one water, a
  pan is shore, past the unit is not). **THE RIM GUARD**: the carve knows nothing of lakes - an outlet's floodplain and
  valley wall cut the rim away and the lake's sheet stood in the air. Beside a lake (within 3 px; not on its wet
  pixels, not inside a river's channel, which must cut through) the ground is kept at the nearest lake's level + 0.1
  model m at the shore pixel, that floor falling at "Valley slope" farther out, never above the uncarved ground (only
  the unit the point lies in is searched). `riverSpeed` = the most influential piece's Manning speed
  (as RiverSystem's `segmentWater`; 0 on a dry bed); and `river` = 1 over a lake's wet pixels fading to 0 within
  "Lake shore (px)" of them (default 0.75) (the beach on the bed and the shoreline, no grass / clutter under the water). `river` = 1 in the channel fading to 0 at the floodplain's
  edge. **`waterLevel` stays the sea's** (TerrainPoint, `ETerrainWater`), so the ocean, its swash, its buoyancy and the
  bake's water channel never see inland water - no gate in the bake. Coarse queries pass straight through.
* **THE WATER HUMIDITY** (V4's riparian boost, the user 2026-10-10): `apply` pulls `TerrainPoint::humidity` toward 1 by
  "Water humidity" x a factor that is 1 at a perennial river's channel edge (x smoothstep(Q / "Water humidity full Q"))
  or a lake's shore, fading (smoothstep) to 0 over "Water humidity spread" (engine m) - the max over the rivers and lakes near the point.
  The sampled climate only: the units are routed from the raw tile planes, so it never feeds back into the rivers.
  Everything reading the wrapped sampler at Full detail sees it (the data map's near cascade - terrain textures, grass,
  clutter - trees, rocks); the far cascade samples Coarse (no rivers), so it fades out where that takes over. A river
  only reaches as far as its pieces are listed in the cell (its carve reach); the lake search runs out to the spread (the
  rim guard keeps its own 3 px).
* **The terrain mesh carries the river to the GPU**: `generateChunk` writes `TerrainPoint::river` into the vertex's u
  and 1 (inland water / dry bed) into its v - not a texture coordinate any more. The splat paints the bed with the
  beach layer and the grass / clutter culls read the same value (RendererVK CONTEXT "Terrain and ocean integration").
  Its resolution is the drawn node's vertex spacing (2 m at LOD0).
* TreeWorld's water term (trees AND rocks) takes `max(sea, inlandWater)` where there is inland water, so nothing places
  in a lake. **A river's bed is kept clear by its INFLUENCE instead** ("Vegetation clear": no tree or rock where
  `river` is above it), read from its OWN 1 m grid (`ITerrainSampler::sampleRiverGrid`: the influence alone, from the
  river units, no terrain - cheap), the MAX of the cell's corners. At mpp 5 most beds are 1-3 m wide: the 8 m terrain
  field's points (and even its corner max) missed them, and trees stood in them.
