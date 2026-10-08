# Rivers and lakes: generation and rendering plan

Status (2026-10-08): **V0 and V1 built** (V0: the coarse network + the lake balance, drawn on the lobby preview; V1:
the units as debug lines; Procedural CONTEXT "Rivers"). Differences from the plan:

* the coarse network is NOT disk-cached (its inputs are; a domain routes in milliseconds);
* a depression under the breach limits passes its water through without a carve (carving is V2);
* units are a fixed N everywhere - "the bounded area as one unit" (11.1) is not built;
* no meanders yet (4.6.3, decision 11.5 open): the paths are the smoothed D8 paths;
* the outlet agreement blend (4.3.6) is not built: a crossing keeps the coarse Q, the upstream unit its fine Q;
* the per-unit segment GRID (4.7) is not built - V2 builds it for the sampler's lookups;
* a unit is disk-cached only when all the tiles it reads lie inside the generated bounds;
* 4.4 / decision 11.3: the unit edge away from the crossings is a SOFT wall ("Edge wall" m), not a wall - a stream
  whose basin really drains across it ends at the edge, and a basin spilling through it is never a lake. Hard walls
  made lakes far above the ground, cut off at the unit edge. Approach B of the river discussion: a drainage network computed
from the diffusion field, refined against the full-detail tiles and CARVED INTO THE SAMPLER, so every terrain
consumer gets the river from the one datum. Approach D (authored / gameplay rivers) is out of scope.

## 1. Goals

* Rivers that drain the terrain the diffusion model makes: they start in wet high ground, join, grow downstream
  and reach the sea or a lake. Lakes in the depressions the model leaves.
* **Climate-driven** (the user, 2026-10-08: use the humidity field): the model's precipitation and temperature set
  how much water each cell gives (runoff), so wet regions have dense networks and full lakes, and dry regions
  have few rivers, dry beds and terminal lakes / salt pans (section 6).
* **Part of the terrain field**: `ITerrainSampler` reports the carved bed, the water level and the flow
  direction. The chunks, the collider, both bakes, the tree / rock records and the ocean shore map see it with
  no change of their own.
* **Pure and deterministic**: a function of (seed, model data, river config) only. The same river at a spot,
  whatever the camera did before. Cached on disk like the tiles.
* **In the model frame**: the network (catchments, discharge, widths) is computed in MODEL metres, so
  `metersPerPixel` scales the rivers with the world, like every other length (the uniform-scale rule).
* Per-frame cost first ([[runtime-over-load-cost]]): all the hydrology is load-time work.

Out of scope: authored rivers, river gameplay (Nav costs, fords, bridges), physics currents, river audio. See 10.

## 2. The facts the design rests on

At the default `metersPerPixel` 5 (`Settings/Public/Terrain.ixx:187`):

| Unit | Model frame | Engine (mpp 5) |
|---|---|---|
| Native pixel | 30 m | 5 m |
| Full tile (`TILE` 256 px) | 7.68 km | 1.28 km |
| Coarse pixel (`nativePerCoarsePixel` 256) | 7.68 km | 1.28 km |
| Coarse tile (`CTILE` 64 coarse px) | 491 km | 82 km |

* **One coarse pixel IS one full tile** (256 native px each, `GeneratorV3.cpp:28-36`, and the npc of the
  shipped config). [Certain] So a flow direction on the coarse lattice is a flow direction from one full
  tile to its neighbour. That is the backbone of the whole design (section 3).
* Coarse tiles are cheap ("a handful of 64x64 model calls"), on disk under `Local/Diffusion/<seed>/`, and the
  lobby preview already generates ~16 of them. [Certain] A full tile is ~1.5 s cold. [Certain]
* Every tile carries `elev`, `macro`, `tempSea` and `precip` (mm/yr) (`FieldTile`). [Certain] The climate
  inputs for runoff exist at both levels.
* The sampler already has the output slots: `TerrainPoint::waterLevel`, `flowAngle01`. `applyFlowField` keeps an
  authored flow direction (`HeightMapBaker.ixx:326`); `applyWaterReach` leaves water above sea level alone.
  [Certain]
* Hydraulic geometry (Leopold-Maddock): width ~ a Q^0.5, depth ~ c Q^0.4. With a ~ 4, a 150 m3/s river is
  ~50 m wide (8 m engine), a 2 m3/s creek ~6 m (1 m engine). [Likely] So at mpp 5, with true proportions,
  creeks and larger are visible; small streams are under a metre and need the width tweak (11.6) or stay as
  bed shading only.

## 3. The model: two levels, joined at tile edges

```
  COARSE NETWORK  (per coarse tile, from a 5x5 coarse-tile domain)
      conditioned coarse DEM -> D4 flow between coarse pixels = between FULL TILES
      runoff-weighted accumulation -> discharge Q on every tile-to-tile link
      big lakes (depressions >= 1 coarse pixel)
                      |
                      |  crossings: (tile edge, point, Q, water level) - decided once, shared by both sides
                      v
  RIVER UNITS  (N x N full tiles, on a fixed model-space lattice)
      native-resolution DEM of the unit's tiles
      outlets / inlets = the coarse links that cross the unit's edge
      hybrid breach/fill seeded from the sea and the outlets -> flow directions
      runoff accumulation + inlet discharges -> Q per pixel; lake water balance
      channels (Q above a threshold) -> segment graph -> smoothed paths -> monotone water profile
                      |
                      v
  SAMPLER  (TerrainGenV3::fill)
      nearest segments / lake mask -> carved height, waterLevel, flowAngle01, detail mask, river fields
```

**Why two levels.** Accumulation is non-local: Q at a point needs the whole catchment upstream, which can be
thousands of model km away. Full tiles cost ~1.5 s each, so a fine-resolution pass over a continent is not
possible. The coarse level has whole catchments for little cost but no fine paths (7.68 km model per pixel).
The coarse level decides HOW MUCH water moves between tiles and through which tile edge; the units decide
WHERE it runs inside the tiles. [Likely]

**Why the units only talk through crossings.** A unit must be computable from its own tiles plus a few edge
tiles, never from its whole upstream catchment of full tiles. So every flow that leaves or enters a unit is
a coarse link, and its discharge comes from the coarse level, not from the upstream unit. [Certain, by
construction] The cost: inside a unit, fine sub-basins whose real watershed does not follow the coarse
directions must be forced (section 4.4).

## 4. Algorithm

### 4.1 The coarse network

Per coarse tile C, from a DOMAIN of the 5x5 coarse tiles centred on C (2 tiles = 982 km model of margin).

1. Assemble the domain's coarse elevation (320x320 px). Sea = elevation below 0 that connects to the domain edge or
   to a large open-water body (not V3's kilometres of -0.05 m film: use the same idea as `applyWaterReach`, a
   minimum depth or area for a seed).
2. **Condition** with a hybrid breach / fill (Lindsay 2016): priority-flood from the sea pixels and the domain
   edge; a depression is breached when the cut is shorter and shallower than the limits, otherwise filled.
   A filled depression above a minimum area is a LAKE CANDIDATE (4.5).
3. **Flow directions: D4**, not D8. A diagonal coarse link would cross between tiles at a single corner point.
   Flats are resolved by the distance to higher and lower ground (Barnes 2014), so a flat drains toward
   its outlet, not in a fan.
4. **Runoff** per pixel from precip and temperature (section 6.1), in m3/s per pixel.
5. **Accumulate** in priority-flood order. A lake candidate does its water balance there (4.5).
6. Keep only C's own 64x64 pixels: per pixel the D4 direction, Q, the filled height, and the lake id/level.

Each coarse tile ALWAYS uses its own domain, so the result is deterministic. Neighbouring coarse tiles use
different domains and can disagree on the Q of rivers whose catchment is larger than the 2-tile margin
(continent-scale rivers). Where a link crosses a coarse-tile edge, both sides take the value of the
DOWNSTREAM tile's domain, so the jump becomes a jump in the upstream tile's width at its coarse-tile edge
(82 km engine apart). [Likely] The CPU cost is trivial (100 k px). The cost is the 25 coarse tiles per
domain; they are shared between domains and cached on disk. [Likely]

Disk: `Local/Diffusion/<seed>/rivercoarse_x<j>_z<i>.rvc` (versioned, keyed by the river config hash).

### 4.2 The units and crossings

**A unit = N x N full tiles** on a fixed lattice in model space (N is decision 11.1; 4 = 5.1 km engine at mpp 5).
Bigger N means fewer forced crossings and a longer cold wait before the first river shows.

**A crossing** is a coarse link (tile p -> tile q, D4, Q above the smallest channel threshold) where p and q are
in different units. Its point on the shared 256-px tile edge: the lowest point of `min(edge strip of p,
edge strip of q)` within a window around the point where the coarse flow line crosses the edge. The two
units compute it from the SAME two tiles, so they agree bit for bit. [Certain, by construction] A unit
therefore loads its own tiles plus the outside tile of every crossing on its edge. A crossing tile outside the
generated bounds does not exist: the crossing is then decided from the inside tile alone, and only the inside
unit uses it.

At the crossing, Q is the coarse link's Q, and the water level is bed + depth(Q), so both sides start and end
the river at the same height and width.

Links between tiles INSIDE one unit are not crossings: the fine routing decides those paths freely.

### 4.3 Fine routing inside a unit

On the native-pixel DEM of the unit (N*256 squared, model elevation; no detail noise, because the channel masks it):

1. **Seeds**: the sea pixels and the OUTLET crossings. Every other edge pixel is a wall, except the inlets.
2. **Hybrid breach / fill** priority-flood (as 4.1.2, with fine limits). Lake candidates come out of the fill.
3. **Flow directions** D8 (paths inside the unit can be diagonal), flats resolved as in 4.1.3.
4. **Runoff** per pixel (6.1), rescaled per coarse pixel so the fine runoff of a tile sums to the coarse
   pixel's own runoff. This makes the two levels agree on the water budget.
5. **Accumulate** in priority-flood order, plus the inlet crossings' Q at their pixels, plus the lake balance.
6. **Outlet agreement**: the fine Q that reaches an outlet is not exactly the coarse link's Q (another fine
   sub-basin split). The crossing keeps the coarse Q; the last few hundred metres of the channel blend toward it.

Size: N=4 is 1024^2 = 1 M px, ~100 ms of CPU. [Likely] For a bounded session the whole playable area can be ONE
unit (no forced crossings inside) up to a cap of ~16x16 tiles (16 M px, ~150 MB while it runs). Bigger areas tile
into units. Decision 11.1.

### 4.4 Forced crossings: the cost of the decomposition

A fine sub-basin near a unit edge whose real outlet is a WALL (the coarse level says its tile drains another
way) can only drain through the unit. The hybrid conditioning then breaches through the ridge (a gorge) or fills
it (a pond). Two limits control this:

* the breach depth / length limit: past it, the sub-basin is filled, not cut;
* a filled sub-basin below the lake area threshold is a SINK: its water soaks away, no lake is drawn, and
  its streams end in it.

So a small stream near a unit edge can end in a sink or a pond instead of leaving the unit. Decision 11.3. With
bigger units this happens less often (perimeter / area). [Likely]

### 4.5 Lakes and the water balance

A filled depression above the minimum area (per level) is a lake. In priority-flood order:

* Q_in = the accumulated inflow. The evaporation from the open water, E (6.2), times the lake's area at its
  spill level A(L_spill).
* Q_in >= E * A(L_spill): the lake is FULL. Its level is the spill level, and Q_out = Q_in - E * A goes on
  downstream through the spill point.
* Otherwise the lake is TERMINAL (endorheic): its level L < L_spill where E * A(L) = Q_in (A(L) comes from
  the fill's hypsometry, which the priority-flood gives for free). Q_out = 0. Very low Q_in with a large basin is a
  DRY PAN (salt flat): no water, a pan surface.

Lakes from the coarse level (>= 1 coarse pixel) are the same lakes in the units, with the coarse level as their
level and the unit refining the shoreline. Smaller lakes exist only in the units.

### 4.6 Channels, paths and the water profile

1. **Channel pixels**: Q >= `Q_perennial`; between `Q_ephemeral` and it (or with a low runoff ratio, 6.3) the
   channel is EPHEMERAL: a dry bed.
2. **Graph**: trace from every head downstream to junctions, lakes, crossings and the sea -> segments with Q,
   Strahler order, the ephemeral flag.
3. **Paths**: D8 paths are 45-degree staircases. Smooth them (Chaikin, then a Catmull-Rom fit) inside a corridor of
   a few pixels, endpoints pinned. On a resolved FLAT (a floodplain, a filled valley floor) the D8 path is
   artificial, so a meander curve replaces it: a sine-generated curve (Langbein) with wavelength ~11 x width, and a
   sinuosity from the valley slope and the climate (6.4), clipped to the valley floor. Decision 11.5.
4. **Profile** per segment, from its downstream end (a crossing, a junction, a lake level, the sea):
   * bed(s) = the unit DEM along the path, in model metres;
   * water W(s) = walking UPSTREAM, max(W_downstream, bed(s) + depth(Q(s))): never lower than its downstream
     neighbour, so the surface only descends downstream;
   * the carved bed = W(s) - depth(Q(s)); where the terrain was higher than that (a breach), the cut is a gorge.
5. **Junctions**: the tributary ends at the main river's W. If the tributary's own W is higher there, the drop is a
   step in its profile (a hanging valley). A step steeper than `fallSlope` is a FALL marker; a steep reach is a
   RAPIDS flag.
6. **Mouth**: W ends at `seaLevel()`; the last reach widens (an estuary, by Q) and the flow direction blends into the
   shore flow (`applyFlowField` already blends authored directions into the surf).

### 4.7 What a unit stores

* Segments: polyline points with (position, W, half-width, depth, Q, flags: ephemeral / rapids / fall), in model
  space.
* A segment GRID (e.g. 64 m model cells) listing the segments whose carve reaches each cell: the per-sample lookup.
* Lakes: id, level, kind (full / terminal / pan), a bit mask at native resolution (1024^2 bits = 128 KB at N=4) dilated
  one pixel, and the shoreline polygon for rendering.
* Fall and rapids points (for the renderer).

Disk: `Local/Diffusion/<seed>/river_x<j>_z<i>.rvu`, versioned (`RIVER_CACHE_VERSION`) and keyed by the river config
hash. Built under a per-unit `JobEvent` like the tiles (one builder, concurrent requesters park). The build fetches
tiles, so it parks fibers on the pipeline `JobMutex` exactly like a tile miss; the routing itself runs unlocked.

## 5. Integration into the sampler

`TerrainGenV3::fill` gets a river step after the height:

1. **Resolve** like tiles: `sampleGrid` resolves the RiverBlock (the units the grid touches) once; the point path
   resolves one unit. An unresolved unit (outside the bounds, cache-only with no file, coarse detail) = no fine
   rivers there (5.3).
2. **Query** the segment grid cell: the nearest segment(s) inside their influence radius. Per segment: the distance d,
   the profile at the projected s (W, half-width w, depth D, Q).
3. **Carve**, model frame, then through `vertScale`:
   * channel |d| < w: bed = W - D * profile(d / w) (a parabola, flatter for a braided reach);
   * bank w .. w + bankWidth(Q): blend from the bed to the terrain;
   * valley floor (wider, by Q and the valley slope): pull the terrain DOWN toward W + floodplain height;
   * every term only lowers the terrain (min), so two rivers near each other compose by min.
4. **Detail mask**: the slope-masked crag noise times (1 - valley weight), so no crags in the channel.
5. **Outputs**: `waterLevel` = W inside the channel (and the lake level inside a lake mask where h < level),
   `flowAngle01` = the segment's direction, new fields (below).

`TerrainPoint` gains:

| Field | Meaning |
|---|---|
| `waterKind` | Sea / Lake / River / None - the consumers that must treat inland water differently (the bake's G for the ocean, 7.1) |
| `river` | 0..1: 1 in the channel, fading over the bank and the valley floor - the placement and shading input |
| `riverQ` | discharge (or its log), for the bed material and the placement |
| `dryBed` | the ephemeral / pan flag |

### 5.1 Consumers that get it for free

The chunks and the collider (carved heights). The tree / rock records (they skip under-water points) [Likely - to
check per system]. The flow bits of both bakes (authored directions are kept). `applyWaterReach` (water above sea
level is left alone; river texels near the mouth must not SEED reach: the `waterKind` check).

### 5.2 Carve and the terrain LODs

The carve is a pure function of (x, z), never of the grid step. **It must be**: the edge stitch interpolates the
finer node's OWN samples at the coarser node's lattice points (`TerrainGenerator.cpp:80`), so a step-dependent carve
would open cracks. [Certain] The cost: a channel narrower than a node's vertex spacing (LOD0 2 m, LOD2 8 m, LOD4 32 m)
aliases at distance. The surface renderer handles that (7.2, 7.3: it fades a ribbon out where the drawn terrain LOD cannot
hold its width), and the river map shades the bed at any distance.

### 5.3 Coarse detail, bounds and the far cascade

* `ESampleDetail::Coarse` (the far cascade, the preview) reads the COARSE network: coarse lakes (water level) and, for
  the preview, the river lines. No carve (7.68 km model per pixel).
* Outside the generated bounds there are no full tiles, so no units: only the coarse lakes. Decision 11.7: also draw
  coarse-level river surfaces past the bounds, or nothing.
* Lakes and rivers are NOT in the terrain-data map's water channel (7.1), so neither cascade's resolution limits them:
  the water pipeline draws them from the unit data at any distance the units are resident.

### 5.4 Cost per sample

One more cell lookup and a few segment distances per point. A `sampleGrid` of 129^2 resolves its units once. [Likely:
small next to the bilinear tile read and the detail fBm; to measure with `Tools/profile.ps1`] The point path takes the
unit cache lock next to the tile cache lock it already takes.

## 6. The climate inputs (humidity and temperature)

All in real units, from the tile planes (`precip` mm/yr, `tempSea` + the lapse at the pixel's elevation). The
constants are tweaks; the defaults below are first guesses. [Guessing]

### 6.1 Runoff

Annual runoff R = P - ET, with ET from the Budyko curve (Fu's form):

    PET = kPet * max(0, T + 5)                       mm/yr    (kPet ~ 45: 25 C -> 1350, 0 C -> 225)
    ET  = P * (1 + PET/P - (1 + (PET/P)^w)^(1/w))    w ~ 2.6
    R   = P - ET                                     mm/yr, >= 0
    q   = R / 1000 * pixelArea / 3.156e7             m3/s per pixel (pixelArea = 900 m2 native, model frame)

Wet and cold regions give most of their rain as runoff; hot dry ones give almost none. This alone gives dense
networks in humid zones and sparse ones in deserts, with no area threshold to tune per biome. [Likely]

### 6.2 Open-water evaporation

E = kLake * PET (kLake ~ 1.1), per m2 of lake. Used by the lake balance (4.5). Hot dry basins hold terminal lakes and
pans; humid ones fill and spill.

### 6.3 Losing streams and dry beds

* **Transmission loss**: where the local R is ~0 (arid), the channel loses water along its length:
  Q_out = Q_in * exp(-L * kLoss * aridity), aridity = PET / P. A river from wet mountains can still cross a desert
  if it is big (an exotic river); a small one dies out in an alluvial fan.
* **Ephemeral**: the runoff ratio R/P below a threshold, or Q between `Q_ephemeral` and `Q_perennial`: a dry bed.
  It is carved and shaded (gravel / sand) but has no water surface.

### 6.4 Channel form

From (valley slope S, Q, aridity):

* humid, gentle slope: single-thread, MEANDERING, sinuosity up to ~2, narrower and deeper (vegetated banks);
* arid or steep with a high Q: BRAIDED, wide and shallow (a flat parabola, w x 1.5..3, depth x 0.5), sinuosity ~1;
* steep and small: straight, step-pool (rapids flags).

### 6.5 Riparian humidity (an option)

Near a channel the ground is wetter than the regional climate. `fill` can raise the reported humidity by
`riparianBoost * river` (the `TerrainPoint::river` field, section 5). The terrain splatting, the tree / rock / clutter climate boxes and the fog all
read humidity, so a desert river gets a green corridor with no new rule anywhere. Off by default; decision 11.8.

## 7. Rendering

### 7.1 The ocean is NOT changed

The user (2026-10-08): the ocean is at its maximum complexity; rivers get a NEW pipeline, and the ocean is not
extended for them. So:

* **Neither rivers nor lakes go into the ocean.** The bake writes G (the water level the ocean clipmap, its swash gate
  and its buoyancy ride) from `waterLevel` only where `waterKind` is Sea. A river or lake texel bakes as dry ground
  for the ocean. This is a `HeightMapBaker` change, not an ocean change. [Certain that it must; the bake site to find]
* **Lakes are drawn by the new pipeline too**, as flat meshes from the unit's shoreline polygons (7.2). This also
  removes the near-cascade limit: a small lake is visible as far as its unit is resident, not only inside the 4 km
  near cascade.
* `applyWaterReach` and `applyFlowField` see the same G, so they also see no river or lake water. The river's flow
  direction still reaches the flow bits through `flowAngle01` (authored directions are kept).

### 7.2 The water pipeline: `RiverSurface`

A NEW pipeline and shader pair (`river_surface.vs/fs.glsl`), separate from `ocean.*` and `terrain_film.*`. Two
kinds of mesh, both built by a CPU job per unit when it comes near the camera, one `RenderMesh` per unit (as the
terrain nodes):

* **River ribbons**: vertices across the width at W, UV.x across, UV.y = the distance along the river (for the
  flow), plus Q / speed / flags per vertex.
* **Lake surfaces**: the shoreline polygon at the lake level, triangulated, a little larger than the wet area (the
  terrain occludes the excess). Terminal lakes use their balanced level; pans have no mesh (a terrain material).

* **Shading**: written for flowing and still inland water, NOT a copy of the ocean: flow-mapped normals (two
  phases, the speed from Manning: v ~ Q^0.1 S^0.3; a lake has speed 0 and only wind ripples), absorption and
  turbidity by depth (the scene depth under the surface), reflection, foam where the speed is high, at rapids and
  falls, at junctions and along the banks. It may `#include` the shared lighting / sky / cloud-shadow includes the
  film and the ocean already use (atmosphere, `cloudSunTransmittance`, the reflection-ray sky), which are not
  ocean logic. [Likely]
* **Draw order**: a transparent surface after the opaque terrain, the grass, the clutter and the film (it reads the
  scene depth and colour under it). Its shadow / GI / TLAS presence: none at first (the water is a thin surface).
* **Edges**: the ribbon is a little wider than the wet width; the carved bank occludes the excess (the bank crosses W at
  exactly w by construction, section 5 step 3).
* **Distance**: the VS evaluates `terrainLeafLod` (the terrain's own LOD function, mirrored bit for bit) at the vertex
  and fades a river whose width is below k x the drawn node's vertex spacing. So a ribbon never floats over terrain
  that cannot show its channel. The film takes over there (7.3).
* **Falls**: a vertical ribbon piece at each fall marker, with foam and spray in the shader. **Procedural cannot link
  Particle** (Dependency direction). Real particle spray needs the fall points to go up to App / Entity: a later item.

### 7.3 Narrow streams: the terrain film

The user (2026-10-08) is fine with re-using the terrain wet FILM (the overlay pass over duplicate terrain geometry,
`terrain_film.*`) for rivers. It takes what the ribbons cannot hold:

* **streams narrower than the ribbon threshold** (below ~1-2 m engine, most first-order streams at mpp 5): the river
  map (7.4) feeds the wetness field as a full-fill water source along the channel, so the film draws a shallow
  running stream in the carved bed. Its flow follows the river map's direction and speed, not only the slope;
* **the distance fallback**: where a ribbon fades out because the drawn terrain LOD is too coarse, the film's
  water in the channel stays;
* **wet banks and splash zones**: the bank-wetness term of the river map.

The film follows the ground, so it shows a stream as a thin sheet over the bed, not as a flat surface across the
channel. That is correct for a shallow stream and is why the ribbon takes over above the width threshold. [Likely]
The film change is an input (the river map as a source, its flow direction) - its surface and shading stay as they
are. [Likely: to check the film's flow input]

### 7.4 The river map (GPU)

A camera-centred map, ~1 m texels over ~512 m (like the forest floor map), splatted by a compute pass from the
segments of the units near the camera: signed distance to the water's edge, W, the flow direction and speed, Q, the
dry-bed flag, a bank-wetness term. Consumers:

* the terrain shader: the bed material (gravel / sand / mud by speed and climate; dry beds), wet banks;
* the grass: none in the channel, fuller on the bank;
* the clutter: a `River` term in the `.clutter` Placement (pebbles on bars, reeds - 8);
* the wetness pipeline and the film: the stream water source, its flow direction and speed, the bank wetness (7.3);
* the `RiverSurface` shader: the shore distance for edge foam.

## 8. Placement

* **Trees / rocks** (CPU records): a `River` ground term in the `.tree` / `.rock` Placement blocks, from
  `TerrainPoint::river` / `riverQ`: riparian species (willow-like bushes), boulders in steep channels, nothing in the
  water. They also see `dryBed`.
* **Clutter** (GPU): `River` and `Bar` terms from the river map (7.4): river stones, sand bars, reeds.
* 6.5 is the other half: the riparian humidity changes which climate box wins next to a river.

## 9. Tweaks and invalidation

"Terrain/Rivers" in `Settings.Terrain`: enabled; the runoff constants (6.1-6.3); the thresholds `Q_perennial` /
`Q_ephemeral`; breach limits; the lake minimum areas; hydraulic geometry a / c and `widthScale` (11.6); bank / valley
widths; meander strength; riparian boost. The river config joins `TerrainConfigV3` (operator==, so a tweak rebuilds),
and its hash keys the river caches: a change rebuilds the units, never the tiles. The look tweaks (foam, speed scale,
colours) are renderer-side and change nothing in the caches.

## 10. Phases

| Phase | Content | Proof |
|---|---|---|
| **V0** | Coarse network (4.1) + the lakes balance, drawn on the lobby `TerrainPreview` (river lines by Q, lakes, pans) | The preview: the networks look right per climate |
| **V1** | Units: crossings, fine routing, channels, profile (4.2-4.7), disk cache; world debug lines (`DebugLinePipeline`) | Lines follow the valleys, meet at the edges |
| **V2** | The sampler carve + `waterLevel` / `flowAngle01` / the new fields (5); the bake's sea-only G gate (7.1) | Carved channels and dry lake basins, no ocean in them, collider |
| **V3** | The `RiverSurface` pipeline: ribbons + lake meshes (7.2); the river map + the terrain bed shading (7.4) | Water in the channels and lakes |
| **V4** | The climate expression: dry beds, terminal lakes / pans, braided vs meandering, riparian boost (6.3-6.5) | Deserts vs humid regions |
| **V5** | The film for narrow streams + the distance fallback (7.3); placement terms, grass, clutter (8) | Streams, riparian ground |
| **V6** | Falls and rapids, far rivers past the bounds (11.7), distance LOD of the ribbons | |

Later, not in this plan: Nav / Game (water depth as a cost, fords, bridges), buoyancy and a current force in rivers,
river audio, particle spray at falls (all need data passed above Procedural).

## 11. Decisions for the user

1. **Unit size N**: 4 (5 km engine, recommended for the sandbox) and the bounded area as ONE unit up to ~16x16 tiles;
   or a fixed N everywhere.
2. ~~River surface~~ DECIDED (2026-10-08): a new `RiverSurface` pipeline for rivers and lakes, the film for narrow
   streams; the ocean is not changed (7.1-7.3).
3. **Forced sub-basins** (4.4): end in sinks / ponds (recommended), or let the stream fade out at the unit edge.
4. **Multiplayer determinism**: the extraction is DISCRETE (thresholds, argmins). If two machines' tiles differ by
   centimetres (DirectML is not guaranteed bit-exact across GPUs [Guessing]), a river can take a different path, and
   the collider then differs. Options: accept it (the server is authoritative for physics?), or the server sends its
   river units to the clients (a few KB each).
5. **Meanders** (4.6.3): procedural on flats (recommended), or only the smoothed terrain path.
6. **Width scale**: true proportions (width from the model's Q x worldScale; creeks ~1 m at mpp 5) vs a `widthScale`
   exaggeration and a minimum visible width for small streams.
7. **Past the generated bounds**: coarse-level rivers drawn there (ribbons at coarse paths, no carve), or nothing.
8. **Riparian humidity boost** (6.5): on or off by default.

## 12. Risks

* **Forced breaching looks artificial**: a straight gorge through a ridge. The breach limits and the larger unit size
  are the dials; the debug lines in V1 show how often it happens. [Likely]
* **V3's sea-level film** (kilometres at -0.05 m) can read as sea or as a giant lake; the sea seed rule (4.1.1) must
  reject it, like `applyWaterReach` does. [Likely]
* **Coarse vs fine disagreement**: a coarse link whose fine path climbs (the coarse DEM is a 7.68 km model average).
  The crossing window (4.2) finds the real low point only near the coarse line; a window too small forces gorges, one
  too large moves rivers between valleys. [Likely]
* **The coarse-tile domain seam** (4.1): continent-scale rivers can change width at a coarse-tile edge. [Likely: rare at
  mpp 5, 82 km engine apart]
* **Point-path cost** on the collider and the record pumps: one more cache lookup per point. [Guessing: to measure]
