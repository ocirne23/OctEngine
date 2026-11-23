# The weather and the snowpack

> Part of the RendererVK documentation ([`Docs/Context/RendererVK/CONTEXT.md`](CONTEXT.md)) — read that first.

The weather is ONE global state on top of the world's climate: how hard it rains or snows, and how much warmer or colder
the air is. The renderer splits the precipitation into rain and snow by the air temperature at each place, wets the
ground with the rain (the terrain wetness) and builds a SNOWPACK from the snow. Settings `Globals::settings.weather`
(`Settings.Weather`, tweaks "Weather" and "Weather/Snow").

## The weather ("Weather")

* **The state rows are Synced** ("Precipitation", "Temperature offset (C)", "Day-night swing (C)", "Time scale", the
  snow/rain band, the "Weather/Snow" rates): a client builds the same snowpack as the server from the server's values.
  The co-op game writes the precipitation and the temperature offset per night (the authority; "Game/Weather", see
  The weather in [`Docs/Context/Game/Modes.md`](../Game/Modes.md)). The particle and look rows ("Rain wetting", "Storm
  cloud cover", "Snow/Enabled", "Full cover depth") are local.
* **The air now** = the climate's temperature at the height in question (`terrainTemperatureAt`: the terrain-data map's
  sea-level baseline + the world's one lapse rate) + `m_weatherAirOffset`: "Temperature offset (C)" + "Day-night swing
  (C)" x clamp(2 x the sun height, -1, 1) - warmest with the sun overhead. GLSL `u_weather_airOffset` (live).
* **Snow or rain:** the share that falls as snow is `1 - smoothstep(snowBelow, rainAbove, air)` ("Snow below (C)" 0.5,
  "Rain above (C)" 2: sleet between; `SnowPack::snowFraction`, `u_weather_snowBelow` / `u_weather_rainAbove`, ordered on the
  CPU).
* **At the camera** (`Renderer::getWeatherAtCamera`: x = snow, y = rain, 0..1): the precipitation split at the ground
  under the camera. Procedural pushes that ground's climate temperature every frame (`setCameraTemperature`, from
  `ITerrainSampler::sampleTemperature` next to `setCameraGround`); no terrain = a mild 10 C. **The particle system's
  rain and snow volumes follow it** (the volume exists from 1 % and its count is the share; the "Particles/Rain" /
  "Particles/Snow" toggles force it on at full count - see Weather volumes in
  [`Docs/Context/Particle/CONTEXT.md`](../Particle/CONTEXT.md)).
* **The clouds close:** the coverage in use is at least "Storm cloud cover" x the precipitation (`cloudCoverage`,
  RendererUbo.cpp; over the user's coverage and the co-op shroud's override alike).
* **The rain wets the ground**: the wetness pass adds "Rain wetting (1/s)" x the precipitation per second
  (`u_terrain_wetWeatherRain`, on the wetness tick) x the RAIN share at the texel's own air temperature (the map's
  climate at the map's height + the air offset) x the site map's catch (none under a roof) - so a valley gets wet while
  the mountain above it gets snow. Under
  water all of it counts; without a terrain-data map all of it falls as rain. "Terrain/Water/Rain (1/s)" stays a uniform
  extra.
* `advanceWeather` runs on main in `kickBeginFrameJob`, after `advanceSunCycle` (it reads the sun height).

## The snowpack (`SnowPack`, `Data/SnowPack.ixx`)

**A TABLE, not a map:** the snow depth on the ground (m) per CLIMATE temperature at the shaded height and per CATCH
(how much of the snowfall a place keeps; 1 = open flat ground). The weather is one global state, so every place with the
same climate temperature and the same catch has had the same history: the table is the whole world's pack, near and far.
Nothing is integrated on the GPU (no R16F rounding, no fixed tick, no clipmap window to fill when the camera moves).

* **Layout** (`RendererVKLayout::SNOW_TABLE_*`): 64 temperature bins over -40..+40 C (1.27 C apart) x 16 catch bins
  over 0..2.5 (catch 1 falls on a bin), one float per cell, catch-major `[c * 64 + t]`, 4 KB; the state after it.
* **The step** (`SnowPack::advance`, in doubles: a slow snowfall adds micrometres per frame): game hours = min(sim
  delta, 0.25 s) x "Time scale" / 3600 (the global pause holds it; independent of the sun cycle, which the profiles pin).
  Per cell: + "Snowfall (m per hour)" x the precipitation x the snow share at that cell's air x the catch, then x
  exp(-settling x hours) ("Settling (per day)", the share of the depth per game day), then - the melt: "Melt (m per hour
  per C)" x the air over 0 C + "Sun melt (m per hour)" x the sun height, faded out in cold air (smoothstep(-8, 0, air)),
  clamped to [0, "Max depth (m)"]. At the defaults (time scale 60, 0.02 m/h) a co-op night of full precipitation lays
about 0.2 m.
* **The GPU copy**: present copies the depths into this frame slot's host-visible table (`uploadSnowTable`, next to the
  terrain ray table), read by device address - `u_snow_table.xy` (live), no descriptor. `snow.inc.glsl`:
  `snowPackDepth(tempC, catch)` (bilinear, 4 loads) and `snowPackCover(tempC, catch)` = smoothstep(0, "Full cover depth
  (m)", depth) (`u_snow_invCoverDepth`, lockable) - 0 without a read while `u_snow_enabled` (lockable: locked off, the
  branch compiles out) or `u_snow_active` (live: the pack holds snow) is 0.
* "Snow/Clear snow" empties the pack (a button: the renderer resets it). "Snow/Enabled" off stops the drawing only; the
  pack keeps integrating.
* **The state** (`SnowPack::CellState`, per cell, visual only; integrated in doubles like the depth, a float copy for the
  upload): the surface's grain diameter (mm), meltwater (0..1) and grime (0..1), stepped with the depth. New snow renews the
  surface (1 - exp(-fallen x catch / "Surface layer (m)"): fresh grains of "Fresh grain size (mm)", no grime, dry; a bare
  cell that gets snow again starts fresh); the melt wets it (the melt / "Meltwater hold (m)", x smoothstep(-4, 0.5, air):
  in sub-zero air the sun's melt refreezes in the surface; rain on the pack soaks it in about half an hour); the
  meltwater relaxes toward that rate x "Meltwater drain (hours)" above 0 C or "Refreeze (hours)" below, stepped exactly;
  dry grains grow as sqrt(d^2 + "Grain growth (mm2 per day)" x exp(0.07 min(air, 0)) x days), wet ones as cbrt(d^3 +
  "Wet grain growth (mm3 per hour)" x meltwater x hours) (corn snow); "Grime (per day)" settles, "Melt grime" x the melt
  per surface layer concentrates it. A melted-out cell KEEPS its state (the melting edge's bilinear neighbours read old
  snow, not fresh), and its meltwater keeps decaying: the ground stays wet as it melts out (see The look). The GPU reads it as a vec4 per cell after the depths
  (`SNOW_STATE_OFFSET`, `u_snow_stateTable`: `snowPackCell`, bilinear as the depth; 20 KB per slot in all).
* **Not saved, not synced**: a peer integrates from the Synced weather, so a late joiner or a loaded save starts with an
  empty pack.

## Where the snow shows

`snowW` - the splat's snow layer - is `max(cold x humid, pack) x holds`: the climate's PERMANENT snow (the old rule,
"Terrain/Textures/Snow ...") or the pack, whichever is whiter, on the slopes that hold snow (`snowGroundHolds`: "Snow
slope start / full" on the smooth normal). The pack needs no humidity (it is the snow that did fall), but only DRY ground
keeps it: `depth = snowGroundDepth(T, pos)` (the table at the SITE MAP's catch, below) x `snowGroundDry` (smoothstep(-0.1,
0.2, height over the local water level) x (1 - smoothstep(0.9, 1, the river influence)): the seabed, a river channel and
the ground under the water stay bare), and `pack = snowCoverFromDepth(depth)`. The permanent snow takes no shelter on the
ground. The temperature is the climate's at the shaded height (per vertex), so the snow line follows the height. Three
places compute it:

* `terrainLayers` (terrain_splat.inc.glsl): so every consumer of the splat's coverages follows - the terrain FS (and
  its tessellated variant), the TES height composite, the film's relief depth, the far grass tier, the water's seabed
  splat. The slope rule is folded into the depth (depth x holds x dry), so the cover and the VS's lift read one depth. It
  also gives **`snowBury`** = `max(cold x humid x holds, pack x saturate(depth / "Bury depth (m)"))`: what the snow
  BURIES of what grows there - the grass (`grassTerrainCover`: the cull's density, the blades' size, the far tier) and the
  clutter (the cull's density, `s_cover.w`). The DEPTH, not the cover: a blade taller than thin snow pokes through ("Bury
  depth" 0.3 m buries it all); with "Depth" off nothing sinks and the bury is the cover. `snowDepth` (m) is kept too: the
  far tier's and the reflected grass's canopy is its height MINUS the depth, and its root occlusion is x (1 - cover) (the
  snow covers the roots). **The early return** (the layers under full snow are not computed) waits for `snowBury`, not
  `snowW`: under white but thin snow the grass and the clutter still need the ground's coverages and climate pick.
* The rock FS (`instanced_indirect_rock.fs.glsl`): its copy of the rule with each face's own slope and the vertex's
  temperature at its own height - rocks, dead wood and cacti.
* The objects' layer (`snowApply`, below).

## The depth (`snowGroundLift`)

**The terrain VS raises every vertex straight up by the pack's depth there** (`instanced_indirect_terrain.vs.glsl`, after
the stitching and the fields: the same depth x holds x dry as `terrainLayers`, at the vertex's temperature and site
catch). One expression for the ground, the film (`TERRAIN_OVERLAY_PASS`: its GREATER_OR_EQUAL still meets the ground,
`invariant gl_Position`) and the tessellated chunks' CONTROL POINTS: the TES interpolates them, so the lift is linear
across a patch, every shared edge agrees, and the relief displaces on top of the snow (its snow layer's height map: the
snow's own small-scale shape). The tracks add to the lift (The deformation, below). "Weather/Snow/Depth" (`u_snow_depth`,
lockable) off: no lift and no tracks, the texture layer only.
The climate's permanent snow has no depth.

* **The lit point near the camera is the ground UNDER the snow**: the VS hands the lift to the tessellation
  (`out_snowLift`, location 5, through the TCS), and the TES puts `out_meshPos` (the FS's `TERRAIN_LIT_POS`) and its own
  layers back on the unlifted ground. So the receiver sits on its own caster in the flat shadow map: a ridge's shadow on
  the snow falls where it would without the lift (a lifted receiver against a flat caster shortened it by depth / tan of
  the sun's height), and the layers read the catch and the water gate at the height the VS lifted from. Past the
  tessellation (75 m) the lit point is the lifted vertex: the shift is under a pixel there.
* **Flat on purpose**: the shadow pass, the terrain ray march, the collider and the grass / clutter roots keep the mesh.
  So what stands on the ground - the grass, the clutter, rocks, trunks, buildings, the units' feet - sinks into the
  snow, and the depth test hides the buried part. The snow itself casts no shadow of its own relief.
* **LOD edges**: a node's vertex normal is taken over its own step, so two LODs disagree on a shared edge vertex's slope.
  The lift's slope rule therefore FADES OUT (holds -> 1) from half of the stitching's full-res band to all of it (in base
  chunks from the draw camera, `u_terrain_stitch` / `u_terrain_stitchBands.x`), before the first coarser edge: both sides
  of every edge lift by the same depth. A far cliff lifts by the depth too (it does not show); the cover keeps its own
  slope rule. A stitched midpoint still differs by the depth's curvature over the stitch span (temperature only: the
  site catch is 1 that far out).
* The terrain VS reads the site map through the bindless array (binding 27, its vertex stage).

## The deformation (`SnowDeformPipeline`, `Terrain/snow_deform.cs.glsl`) - THE TRACKS

Feet, units, walkers and impacts PRESS the snow: a track, a berm around it, a crater. Visual only (the colliders hold no
snow; what walks on the ground walks UNDER the snow surface and dents it down to its feet). **A stamp must come AFTER the
begin-frame job** (`Renderer::addSnowTrack` / `addSnowCrater`: `FrameSubmission::beginFrame` resets the frame's list, so a
call from main before `joinBeginFrameJob` is lost) - the entity pass, the game update, or main after the join.

* **Two maps**, R32UI, TOROIDAL around the scene focus like the site map (slot = lattice & 1023): FINE
  (`SNOW_DEFORM_FINE_TEXEL` 6.25 cm, 64 m: footprints) and COARSE (`SNOW_DEFORM_COARSE_TEXEL` 0.25 m, 256 m: trails,
  craters). The windows move in steps of 64 texels (4 m / 16 m), so the centre is within a step of the focus. 8 MB.
* **A texel** = hi16: the OFFSET from the undisturbed snow surface over [`SNOW_DEFORM_MIN` -2, +1] m (undisturbed =
  `SNOW_DEFORM_UNDISTURBED`, offset 0); lo16: the SNOWFALL (mm, mod 65536) when it was pressed. **The refill is at read
  time**: a track (offset < 0) is `min(0, offset + (u_snow_fallNow - stamp) mm)` - the snow fallen since fills it in, with
  no pass over the map. `u_snow_fallNow` = `m_snowfallTotal` (advanceWeather: "Snowfall" x the snow share at the camera x
  the game hours; reset by "Clear snow") in mm mod 65536. A berm (> 0) does not change. **The rebase**: every 30 m of
  snowfall the maintenance resets the refilled tracks to undisturbed (`Tick::rebase`), before the clock wraps and opens
  them again.
* **The stamps** (`Renderer::addSnowTrack(fromFeet, toFeet, radius)` / `addSnowCrater(centre, radius, depth)`, lock-free
  between joinBeginFrameJob and present): into the frame slot's `SnowDeformer` list (`FrameSubmission`, 4096 x 32 B, by
  device address; the tail drops). Dropped while the maps are off and past 150 m (+ the size) from the focus the windows
  were placed around this frame (`Tick::centre`). A TRACK is
  a ball of `radius` whose BOTTOM sweeps from `fromFeet` to `toFeet`; a CRATER a bowl of `radius` (<= 4 m), at most
  `depth` deep under the surface.
* **The pass** (`recordSnowDeform`, inline in the primary after the wetness, BEFORE the grass / clutter culls and the
  scene; both primaries): to GENERAL, the MAINTENANCE on a frame whose window stepped (or after an activation or "Clear
  snow", or on a rebase: the texels outside the window the map held go back to undisturbed - one thread per texel of
  both maps; a scrolled-in FINE texel takes the coarse texel under it raw, so walking back over a trail finds it again),
  then the STAMPS (one workgroup per stamp over its footprint in both maps), back to SHADER_READ_ONLY_OPTIMAL. A frame
  that records nothing (minimized, no instances) commits nothing: the next one runs the same maintenance, a pending clear
  included. The stamp takes the undisturbed depth ONCE at its centre (the pack at the site catch x the slope rule on the
  terrain's normal over +-1 m x the water gate: the VS's lift) and the ground per texel
  (the site map's top in its window: a roof is a ground, else the terrain-data map). It PRESSES (the offset goes down to
  the ball's or the bowl's bottom, never under the ground: min) and pushes a BERM up in a ring of 0.6 x the radius
  outside it ("Berm": the share of the pressed depth, x 4u(1-u) across the ring; only on undisturbed or berm texels:
  max). **A deep track's wall slopes**: where the feet are deeper than the radius under the surface, the press goes on
  past the ball's edge up to the surface at "Track wall slope" (2 m per m, `u_snow_trackWallSlope`), and the berm's ring
  starts at the wall's top (the footprint grows by depth / slope). A vertical step of that depth in one texel was sampled
  by the tessellation's vertex grid into teeth along the wall. CAS loops (8 tries): the stamps of a frame meet in any order. Skipped: feet over the surface (in the air, on a
  deck), feet a metre or more under a top (inside a building), an impact out of the snow.
* **The readers** (`SNOW_DEFORM_READ`, snow.inc.glsl): an R16G16_UNORM view of each map (MUTABLE images: R = the stamp,
  G = the offset) is ADOPTED into the bindless array (`u_snow_deformFineTex` / `CoarseTex`). `snowDeformTapGrad` GATHERS
  the bilinear footprint (2 gathers), decodes each texel and THEN filters: hardware bilinear would mix the stamp clocks
  (an undisturbed texel's is 0), so a fresh track's edge would read as refilled. The shader picks the footprint (floor)
  and gathers at its SHARED CORNER: there the hardware's fixed-point rounding cannot pick another texel pair (at a texel
  edge a centre-based gather read one texel off - spikes on the track walls). It also gives the analytic bilinear slope.
  `snowDeformAt(xz)` / `snowDeformAtGrad(xz)` = the fine map inside its window, the coarse outside (crossfaded over the
  fine window's outer 5 %), faded to 0 over the coarse window's outer 5 %. `snowDeformApply(offset, lift)` clamps it to
  [-lift, 0.5 x lift]: no press under the ground, no berm on bare ground. **The geometry fades out** with the centre
  view's XZ distance (`snowDeformFade`: full within "Track detail range", none past twice it): farther out the vertices
  are coarser than the maps and a point-sampled track shows as dotted pits. A function of xz only, so every stage and
  both patches of an edge agree.
  * **The terrain VS** adds it to the lift (ground, film, the control points): `out_snowLift` is a vec2 (all, the tracks'
    share).
  * **The TES** adds the tracks BETWEEN the control points: the faded deformation at the vertex minus the interpolated
    tracks' share (0 at a corner; the same from both patches on an edge - its fixed corner order).
  * **The TCS** refines over a track within "Track detail range (m)" (20; `u_snow_trackDetailRange`, lockable, 0 with
    "Tracks" off): an edge whose ends (the VS's tracks) or quarter points (taken in a fixed end order: both patches
    sample the same points) read more than 1 .. 5 cm takes up to twice its factor and a cap of at least 32 (footprints
    need ~6 cm vertices); a track at the patch's middle doubles the inner level, up to 32. The patch cull's margin
    grows by the residual's reach (1.5 x the corners' undisturbed depth). It reads the maps through binding 27 (its
    TCS stage).
  * **The terrain FS normal** (`terrainTessPixelNormal`) tilts by the gradient (`snowDeformGradient`, NOT faded: past the
    geometry's fade the trails still shade): one tap's analytic slope where the TES's footprint is at most a fine texel,
    else forward differences over the footprint (3 taps: they low-pass the map). Also where the relief is off.
  * **The rock shell** (the rock VS and FS) and **the caps** (the prepare: the coarse map) take the tracks too: a foot on
    a rock's top or a roof stamps there, the stamp's ground being the site map's top. See The rock shell and The caps.
* **Live** (`m_snowDeformTick.active`, `u_snow_deformPresent`) while snow lies, "Depth" and "Weather/Snow/Tracks" are on.
  Off, the next activation clears both maps.
* **The feeds**: `GameUnitComponent::tickSnow` (every role: the units and the player capsules, the collider's bottom,
  0.8 x its radius, a stamp per 5 cm moved, a jump of 6 m or more starts a new track; a STANDING unit presses its feet
  again once a second, on a per-unit phase, or the falling snow would bury its dent), the testbed's possessed player (key C:
  `InputControls::stampPlayerSnow`, the same rule on its shape's bottom - the local capsule, or on a client the
  locally-owned player only; main calls it after `joinBeginFrameJob`), the carriers' walkers (Carriers.cpp,
  0.3 m), and every projectile impact (`GameProjectileComponent::destroy` queues, `drawImpactLights` hands over the ones
  under 0.5 s old - a teardown's are dropped: radius max(0.5 x splash, 0.3), depth min(0.1 + 0.25 x radius, 1)).

Not built: slumping (a track's walls stay sharp), puffs of snow on a step, footstep events (the tracks are continuous
trenches), the grass and the clutter bent by a track, a track on an object's snow layer, a saved or synced map.

## The site map (`SnowSitePipeline`, `Sky/snow_site.cs.glsl`) - THE SHELTER

Per texel, straight down from the sky against THIS frame's TLAS: the top SOLID surface's world Y, the share of the snow
the foliage above it lets through, and the highest blocking foliage's height over that top. **The catch** of a surface
point (`snowSiteCatch`, snow.inc.glsl) = that pass where the point lies under the highest foliage (a branch over its
crown's shadow card, a mover over a forest: the open sky), x 0 where a solid top stands more than 0.3 .. 0.8 m over the
point: none under a roof or an overhang, less under a crown, the full share on a roof itself. The table is read at that
catch, so a forest floor collects a thin layer (two crown layers at "Foliage block" 0.7 let 9 % through).

* **The window**: `SNOW_SITE_RES`^2 (1024^2) texels of `SNOW_SITE_TEXEL` (0.25 m) = 256 m, world-snapped and TOROIDAL
  around the scene focus (slot = lattice & 1023; the origin is a lattice coord, `Tick::origin`). Readers sample with
  HARDWARE BILINEAR through a REPEAT sampler, which wraps like the slots: a drip line or a roof edge is a quarter-metre
  ramp. The catch fades to 1 near the window's edge (`smoothstep(0.44, 0.49)` of the distance from its centre in window
  widths, the edge at 0.5: 113 .. 125 m from the focus); past it, and without the map, it is 1.
* **The images**: R32F top Y (`SNOW_SITE_NO_TOP` = nothing over the ground) + RG8 (the pass; the highest foliage over the
  top / `SNOW_SITE_FOLIAGE_RANGE` 64 m), 6 MB, made once
  (`createImages`, initSharedBuffers), cleared to open sky and **OWNED by the TextureManager**
  (`TextureManager::adoptImage` -> `Texture::adoptImage`): every reader samples them through the bindless array
  (`u_snow_siteTopTex` / `u_snow_sitePassTex`), so they rest in SHADER_READ_ONLY_OPTIMAL - the layout every bindless writer
  assumes - and only the trace moves them to GENERAL and back (`transition`, keeping the contents).
* **The tick** (`advance`, in `buildUboTerrain` on the WALL clock: the shelters are the world's, not the sim's): 10 Hz.
  A tick traces the texels outside the last window (scrolled in) and one row slice (lattice row % `SNOW_SITE_SLICES`
  (64) = the tick's slice): a refresh every 6.4 s; the whole window on the first tick after an activation or a jump past
  the window. One dispatch over the window; the untraced texels return at once. **The window is committed by `record`**
  (the last RECORDED trace's origin): a frame that records no trace (no TLAS, no instances, a minimized window) asks again
  on the next one and loses no full trace or scrolled-in strip.
* **The slice refresh only where the camera looks**: the TLAS holds only what this frame PUSHED for shadows or RT, and an
  off-screen building past the Near ball and not up-sun is not pushed. So a slice texel is re-traced only where the
  centre view's frustum holds its column (a 4 m sphere at its last top, `u_frustumPlanes`, the frame UBO at binding 9);
  elsewhere it keeps the value it was last seen with. A scrolled-in or full trace writes every texel: an off-screen roof
  there is missing until it comes into view.
* **Live** while "Weather/Snow/Shelter" is on, RT is on, and snow lies (`snowActive`) or anything falls: the rain's
  wetness is sheltered by it too. `u_snow_sitePresent`. Without RT: no shelter.
* **The trace**: the terrain march, then the SOLID query (opaque instances above it), then the FOLIAGE query above that
  surface, **ALPHA-TESTED** (`rtsCandidateBlocks`, rt_shadow.inc.glsl: the instance, mesh, index, vertex, material
  buffers + the bindless array, binding 10 - a bindless consumer like GI and RTAO, filled at the build, slot by slot
  after): a crown shelters its outline, not its shadow card's square. Each blocking layer stops "Weather/Snow/Foliage
  block" (a lockable push value). Both queries cull `RT_CULL_SNOW`: no bark (the crown is the shelter) and **no MOVERS**
  (a node with `PASS_FLAG_MOVER`: units, players, shots, dynamic bodies, `Mover true` prefabs - see Movers in
  [`Docs/Context/Entity/CONTEXT.md`](../Entity/CONTEXT.md), and every structure on the flying ship; `RT_MASK_MOVER` in
  gi_tlas_instances.cs.glsl): a passing unit
  would otherwise stay a bare patch until its row's next refresh. The rays start 400 m above the focus and run 1200 m.
* **Order**: recorded after the GI step (the TLAS build), before the rain trace (`recordSnowSite`, both primaries). The
  scene's readers sample this frame's map with `u_snow_siteOrigin`; the PRE-SCENE readers (the grass and clutter culls,
  the wetness) run before the trace and read the window it held then: `SNOW_SITE_ORIGIN u_snow_siteReadOrigin`.
* **Its readers**: `terrainLayers` (so the ground FS, the TES, the film, the culls), the rock FS, the object layer, the
  wetness CS (the weather's rain x the catch, with a looser 1.5 .. 2.5 m roof test: its height is the 8 m data map's). The
  grass cull and the wetness have no bindless array: they bind the two images directly (bindings 10 / 11 and 5 / 6, the
  site's own repeat sampler).
* The pipeline is built at its first activation (present, `ensureBuilt`: a ray-query shader).
* **The class** (a third image, R8UI 1 MB, `classView()`, owned by the pipeline, GENERAL for life, compute only: the caps'
  prepare): what the top of a traced texel is (`SNOW_SITE_CLASS_*`): 0 the terrain or nothing, OBJECT (a committed solid
  hit), ROCK (a tree-set piece - custom index bit 23, `RT_CUSTOM_TREE_BIT`: with bark culled and cards / leaves never
  opaque only a rock, dead wood or a cactus - or a stream instance on the rock pipeline, a preview rock). The trace writes
  it fresh, which clears the two invalidation bits.
* **The invalidation** (`Renderer::invalidateSnowSite(minXZ, maxXZ, hide)`, any thread): a world rectangle whose tops
  changed. The Game calls it for a structure's footprint (+1 m) when it spawns, goes to rubble or back, is removed, or its
  walkway piece changes, and once over all of them when a match ends (`StructureSystem::invalidateSnowSite`, `clear`; never
  on the ship: its structures are movers). The renderer queues it three times (now, +0.5 s, +2 s: a new structure is
  pushed and gets its BLAS a little later; 256 entries, past that a request grows the entry due nearest, never later).
  `recordSnowSite` takes the due ones, clipped to the window the map holds (a slot outside it is another place; what
  scrolls in is traced fresh), 32 rectangles a frame (the rest merged into the last), and `recordInvalidate` ORs
  `SNOW_SITE_RETRACE` (+ `SNOW_SITE_STALE` with hide) into the class before the trace (a barrier between the rectangles:
  each is a read-modify-write). A STALE texel holds no cap at once; a RETRACE texel is traced on the next tick whose
  frustum test it passes, whatever its row's slice. While the map is off the requests go (the next activation traces
  everything).

## The objects' layer (`snowApply`, snow.inc.glsl with `SNOW_OBJECTS`)

Called just BEFORE `computeLitColor` (nothing new is live across the light loop) in the lit FS (structures, props, the
near bark; not the blended variant - no snow on glass), the tree FS (bark and leaves) and the voxel trees' lighting FS.

* **The cover** (`snowObjectCover`): 0 unless "Weather/Snow/Objects" (`u_snow_objects`, lockable: locked off, the layer
  compiles out) and a splat set with snow. On a face that holds it (the splat's "Snow slope start / full" on the
  NORMAL-MAPPED normal's y: up-facing crevices first), the climate at the point's own height (the terrain-data map, 4
  fetches near), then `max(permanent x saturate(3 x catch), the pack at the catch x dry)`; the catch is the site map's,
  `dry` the ground rule's water gate (the data map's water level: no snow on a submerged face).
* **The material** (see The look): the pack's state at the cover's catch (`snowObjectCover`'s `cell`, read past its
  early-outs) gives the albedo and the facets' roughness; the snow relief's normal on a world-XZ projection
  (`snowObjectRelief`, 1 fetch) with EXPLICIT gradients (`textureGrad`) of the position taken before the per-pixel early-out
  (a fetch after a divergent branch has no defined derivatives), turned onto the surface's normal (`snowNormalOn`). It
  replaces the albedo, the roughness and the normal by the cover, and takes the metal away. With `LIT_SNOW` (the lit and
  tree FS) it also sets up the snow BRDF (`snowLitSetup`): the glints on what does not move (the lit FS: no world delta)
  and not on leaves; the tree's bark anchors them at its position before the wind's sway.
* **Mesh leaves take the voxel crowns' outer-shell rule** (below), so the tiers agree across their crossfade and a
  vertical leaf card in the canopy holds snow: `treeLeafCrownInterior` (tree_leaf_sun.inc.glsl) reads the voxel tier's
  baked interior at the leaf's grid point (`in_treeGrid`) - the type's voxel material's surface atlas alpha, on the level
  of ~16 voxels a side, 3 fetches, only where snow can lie; 0 without a grid or a voxel tier, or in an empty brick. On the
  shell upN = max(N.y, shell x clamp(1 + 0.15 N.y, 0, 1)), and the scale is "Leaf snow" x the shell.
* **Leaves** (mesh and voxel tiers): no site catch (a crown is its own roof), x "Weather/Snow/Leaf snow" (0.6). The voxel
  tier (`snowApplyUp`) takes the crown's whole OUTSIDE as up-facing: its voxels' mean leaf normal faces up only at the
  tips, while the mesh tier's leaves face every way and the up-facing ones hold snow on the crown's bottom too: so on the
  crown's surface (the baked interior 0, x the voxel's leaf share) upN = max(N.y, outside x clamp(1 + 0.15 N.y, 0, 1)) -
  every face counts as up-facing, the faces straight down included (0.85, under the slope rule's start). Deeper than
  "Weather/Snow/Voxel crown snow depth" (0.05 of the baked interior, 0 surface .. 1 core; faded over 0.15) the leaves hold
  none, so the crown's inside shows through its gaps. It fetches the relief at
  mip 4 (no derivatives between its hits) and has `LIT_SNOW` without glints (`SNOW_NO_GLINTS`).
* **Movers are not told apart**: a unit or a player standing out in the snow collects it on its top faces like a crate
  (the FS has no node bit).

**The far-tree volume** takes the same leaf snow per ray (`farTreeSnow`, the march: see Far-tree volume in
[Vegetation.md](Vegetation.md)): the fresh snow's albedo by the cover x "Leaf snow", every face up, open sky.

Not covered: GI and reflection hits (they show the objects without snow), the clutter's meshes
(the clutter is buried by the ground's snow anyway), and the particles' rain shelter (still the rain occlusion map).

## The rock shell (`snowRockShell`, `instanced_indirect_rock.vs.glsl`)

Rocks, dead wood and cacti (one VS, variant LitRock) SWELL by the snow on their top: after the terrain fields (which stay
the bare rock's: the temperature at its own height, the ground for the contact band), each vertex rises STRAIGHT UP by
the pack at its temperature and catch x the ground's slope rule on its normal x the water gate, faded out over the last
20 % of "Weather/Snow/Rock shell range (m)" (150). Up, not along the normal: along it the snow pushed out sideways over a
top's rim (an overhang) and a dead trunk's jagged break (its splinters' normals every which way) folded through itself.
On a CUT face (dead wood's bare wood: v = 1 + the bark cover, so 1 - bark; a break, a splinter, a sawn top) only a nearly
flat face lifts (the slope rule tightened to ~15 .. 30 degrees), so the splinters keep their shape. The meshes are welded,
so the shell does not crack within a level; a LOD switch only pops. "Rock shell" (`u_snow_rockShell`, lockable) and
"Depth" gate it.

* **The tracks press it**: a foot on a rock's top stamps the deformation maps there (the stamp's ground is the site map's
  top), so the VS adds the same clamped offset as the ground (`snowDeformApply(snowDeformAt(xz), lift)` x
  `snowDeformFade`: only within twice "Track detail range", only on vertices that swell - 2 gathers each), and the rock
  FS tilts the snow's normal by the fine texel's analytic slope (`snowDeformGradient`) x the share x the same fade. A dent
  is as fine as the rock mesh's vertices; walking past a low rock (under 1 m) may press the shell's edge within the
  foot's radius (one map per xz).
* **The catch** is `snowRockCatch` (`snowSiteCatch` with a looser shelter test, 0.5 .. 1 m): the site map's top over a rock
  is its BLAS's level (the finest), so a coarser LOD's vertex can lie a few dm under it on its own top. The rock FS's white
  reads the same catch (no bare bulge).
* **The cull pads the frustum radius** of every LitRock instance (the tree-set rocks and the stream ones, previews) by the
  deepest pack while the shell can swell (`u_snow_rockShellPad`, instanced_indirect.cs.glsl; the LOD pick keeps the radius).
  The shadow VS and the BLAS keep the bare rock.
* No cap forms on a rock (the class).

## The caps (`SnowCapPipeline`, `Terrain/snow_cap_prepare.cs.glsl`, `snow_cap.vs/.fs.glsl`) - THE VOLUME ON OBJECTS

A HEIGHTFIELD over the site map's texels (0.25 m) on every object top: roofs, walls, props. Not the ship (its hull, rails
and structures are movers, landed or not: the trace never hits them). Desktop only, while "Weather/Snow/Caps" and "Depth"
are on, snow lies and the site map holds a trace (`snowCapsLive`).

* **The prepare** (`recordSnowCaps`, inline in the primary right after the site trace, every frame): one workgroup per 8 m
  TILE (32 site texels; `SNOW_CAP_TILES`^2 slots, slot = tile & 31) of the window the map HOLDS (`committedOrigin`: a frame
  whose trace did not record never reads a scrolled-in strip), within "Cap range (m)" (100, at most 105: the data ends at
  ~110 m) + 16 m of the centre view. It loads the tile + an 8-texel apron (top, class) into LDS and writes ONE WORD per own
  texel into a 4 MB buffer (`SNOW_CAP_*`; one buffer for all slots, written after every earlier frame's cap VS):
  * **the depth** s (on every loaded object texel: the edges and the quads read the neighbours' snow) = the pack at the
    top's temperature and its catch (the pass: the top is the column's own) x the slope rule on the TOPS' normal
    (one-sided across a step) x the water gate, x the window's fade (0.38 .. 0.43 window widths from its centre, the edge
    at 0.5: 97 .. 110 m) and the range's (the last 20 %);
  * **the edge**: an object texel whose 4-neighbour holds no cap and is not higher (a roof's edge, the window's), or holds
    one and lies LOWER than the surface's own plane (extrapolated from the opposite neighbour: a steep roof is no edge) by
    more than "Cap max step (m)" (0.3) AND that neighbour's snow (a step its snow buries is no edge). A HIGHER neighbour (a
    wall, a chimney, a crate on the roof) ends nothing: the snow banks against it. The pitch a texel may differ by on its
    own roof (the slope allowance): one texel x the tangent of the slope rule's steepest snowy slope, at most 4:1;
  * **the rounding**: d = the exact distance to the nearest edge texel (a separable search, 7 texels: "Cap edge rounding (m)"
    at most 1.75), the profile p = 1 - (1 - x)^2 with x = d / min(rounding, 2 s) (steep at the rim, flat inside), and the
    cap's height over the top `lift = -sink + g p (s + track + sink)`: g = smoothstep("Caps from depth", twice it, s), an edge
    texel sinks "Cap edge sink (m)" (5 cm) into the top (the texture layer covers the rim line), the TRACKS press it (the
    coarse deformation map: the same 0.25 m lattice; roof tracks exist since the stamps take the site top as the ground).
    Under 1 cm over the top a texel is not RAISED and sinks too (a track to the roof, the fade-in): no cap lies in a top's
    plane, so nothing z-fights with the texture layer;
  * **the quads**: KEEP0 = the 4 corners are object texels, planar within the step (or the lowest corner's snow: a buried
    step) and spread within that + the slope allowance x sqrt 2; KEEP1 / KEEP2 = all the KEEP0 quads inside the 2x2 / 4x4
    block (a coarse quad never bridges an alley);
  * **the draw** (thread 0): a tile with a raised texel (or a KEEP0 quad into the next tile, whose snow it carries), its 8
    neighbours inside the window (the VS reads across its edge), in range and in the frustum (its box: the vertices' texel
    centres, the tops of its quads' corners + the highest lift or 1.5 x the deepest snow + the step) goes into its LOD's
    list (16 B records, by device address): LOD 0 within "Cap full detail (m)" (40, at least 20) of the nearest point, LOD 1
    within twice it, LOD 2 beyond - FINER where the coarser LOD would drop a raised quad (a narrow top: no pop). Its LOD
    goes into the LOD TABLE (one uint per slot, reset each frame). A tile with no object texel writes zero words.
* **The draw** (`StaticMeshGraphicsPipeline`, the FIRST draws of the opaque phase, before the opaque set: the tops under a
  cap fail early depth): one indexed indirect draw per LOD, (32 >> lod)^2 quads of 4 vertices of their own (a static uint16
  index buffer), no vertex input, both faces. The VS (`snow_cap.vs.glsl`) reads the record by gl_InstanceIndex, the 4
  corners' words and tops; every vertex of a quad reads the same corners, so they agree:
  * KEPT: the LOD's KEEP bit at the quad's min corner and a raised corner;
  * BANK (every LOD): the quad crosses a step UP (more than the step x the stride) - the low corners hold the cap, and the
    high corners take the low ones' highest snow where theirs is lower: the cap runs into a wall (hidden by depth) and over
    a buried crate's rim, and leaves no slit at the foot;
  * else collapsed to a point.
  **A corner's height** (`capCorner`) is a function of its lattice point and the LOD only, so every quad and tile agrees:
  the words' height, then **the morph** (CDLOD, LOD 0 and 1: over the last 20 % of the LOD's band a point the next LOD
  lacks slides to the next LOD's surface - the mean of its two neighbours along its odd axis, both odd: the quad's 1-2
  diagonal; not across a step), or on the tile's border next to a tile drawn COARSER (the LOD table) **the stitch**: the
  point lies on that tile's edge, between its two points there as it places them. A tile drawn finer than its distance
  (the LOD clamp) therefore never cracks. The normal: the raw heights at +-stride, one-sided across a drop; a bank quad
  takes its face's.
* **The FS** (`snow_cap.fs.glsl`): the lit core (SUN_SHADOW_FIRST, LIT_DIELECTRIC, LIT_NO_UNDERWATER, LIT_SNOW; RTAO
  kept - the contact shade against a wall) with the object layer's material (the same relief tap, decode, XZ projection,
  state and glint lattice as the texture layer the rim sinks into: the state at the column's catch, as the roof reads it),
  the relief turned onto the cap's normal (the rounded rim shades), the snow share 1.
  The sun's shadow from at least 2 cm over the top (a sunk rim's rays would start inside it). Static: the motion target is
  write-masked; alpha 1.
* **Limits**: only the topmost surface per column (as the snow falls), nothing narrower than three texels (0.75 m: both
  texels of a 2-wide top are edges and sink; a 3-wide top is a one-texel ridge; rails get the texture layer only), no cap
  on movers, masked (LitMasked) or NO_RAYTRACING parts. The BLAS is the finest LOD and the
  texels sample under a ridge, so a roof can poke through a thin cap as its white texture layer. Caps cast no shadow and
  are in no BLAS (the cascades are drawn before the trace). VR: none.

## The look (`snow_brdf.inc.glsl`, the material in `snow.inc.glsl`, `SnowRelief`)

The snow has its OWN material and sun BRDF in every path (the terrain splat, the rocks, the object layer, the caps): no
photo texture. One set of functions, so a cap's rim, the roof's layer under it and the ground next to it match.

* **The relief** (`Procedural/Private/SnowRelief.ixx/.cpp`): the splat's snow slot is GENERATED - a spectral synthesis on
  the torus (integer cycles per tile: every band tiles; a fixed seed) of 2048^2 over one snow tile (10 m at the default
  "Terrain/Textures/Snow uv scale" 0.1, 4.9 mm texels): lumps (2.5 .. 0.8 m, a falling spectrum), wind ripples (14 .. 10 cm,
  across a 30-degree wind, sharp crests) in patches, ridged sastrugi along the wind in wind-crust patches (no ripples
  there), grain (3 .. 1 cm) and 600 small pits. `ensureSources` writes the PNG sources into `Local/TerrainTex/Gen/` once
  (the recipe's version is in the stem `snow_relief_v1`: bump it with the recipe), at the start of the terrain texture
  bake (TerrainStreamer), which bakes them like any set. The normals come from the FLOAT height at the slope the bake's
  displacement gives it ((h - 0.5) x `HEIGHT_SCALE` 0.3 x the default ground depth 0.6 m: a few cm of relief); R = world
  +X, G = world +Z. The set has no ARM map (`noArm`: the roughness alpha a constant, the AO 1) and a flat 16^2 albedo. So
  the snow reads 2 textures on the terrain and the rocks (nor + hao), 1 in the object layer and the caps (nor) - the TES,
  POM, the height blend and the relief readers are unchanged. `snowNormalOn(baseN, xy)` is the ONE decode (the splat's).
* **The material** from the pack's STATE (`snowStateAt` / `snowStateFrom`; the climate's permanent snow and every path
  with the pack off read the fresh state):
  * `snowAlbedo`: the spherical albedo of a weakly absorbing granular medium, exp(-sqrt(gamma x 13 x d_eff)) per channel
    - gamma the ice's absorption (0.31, 0.046, 0.016 /m, from its imaginary index) + grime x (6, 8, 11) /m, d_eff the
    grain x (1 + 2 x meltwater). Fresh 0.1 mm grains: (0.980, 0.991, 0.996); old wet grimy snow greys. C++ twin:
    `SnowPack::surfaceAlbedo` ("snow_freshAlbedo", the splat's placeholder).
  * `snowAlpha`: "Grain roughness" (0.6) toward "Wet snow roughness" (0.15) with the meltwater.
  * The terrain FS replaces the splat's fresh placeholders (`snowSurfaceSample`) after the splat by the snow's share
    (`terrainSplatLayers`'s `snowShare`, the share after the height blend - exact for the linear mix); the rock FS before
    its mix; the object layer and the caps directly.
* **The sun BRDF** (`LIT_SNOW`, `snowSunLightH` in doSunLight on pixels with `g_snowW` > 0; set by `snowLitSetup` before
  `computeLitColor`, the object layer in `snowApplyUp`):
  * the asymptotic radiative-transfer reflection of a snow layer, R0(mu0, mu, theta) x r_s^(u(mu0) u(mu) / R0) with the
    ice grains' phase function: about Lambert with the sun overhead, a bright FORWARD SHEEN with the sun ahead at grazing
    angles ("Forward scattering" scales the phase);
  * the DIFFUSION across the relief ("Soft terminator"): per channel the incidence cosine pulled toward the smooth base
    normal's (`g_snowBaseNoL`; the terrain's is the smooth mesh normal in both variants): a soft terminator, blue farthest
    (0.3 x (0.18, 0.38, 1.0) at 1). It moves light, it adds none. A relief flank turned from the sun passes the facing test
    while its base faces it; the tessellated relief's sun gate opens earlier under snow (to -0.2);
  * the ice facets: GGX at F0 0.018 x "Sparkle strength" (3: the light the grains reflect inside them, near and far
    alike) x the glints' X, capped AFTER the Fresnel term at "Sparkle max (x white)" (`u_snow_glintCap` = max / pi);
  * mixed with the surface's own BRDF by the share (the surface's own lobe keeps its full, uncapped value by 1 - share).
    The local lights, GI and the ambient keep Lambert + GGX with the snow's albedo; a leaf keeps its curved specular
    normal under thin snow (blended to N by the share). The voxel crowns take it too (`SNOW_NO_GLINTS`), so the tiers'
    crossfade keeps the snow's brightness.
* **The glints** ("Sparkle", lockable: locked off, compiled out; `snowGlint`): a discrete-microfacet POISSON estimate on a
  fixed world lattice. lambda = the mirror facets per cell ("Sparkle density" / grain^2, fewer once wet) x the cell's area
  x D x NoH x the half vectors' solid angle (the sun's disc / 4 HoL + the facet cone of "Sparkle spread (deg)"); each of the
  4 nearest cells draws its count from a hash of the cell and the half vector's slope bin (offset per cell), X = the
  bilinear count / lambda, E[X] = 1. The cell (2^-10 m and up) follows the pixel's footprint (`snowFootprint`: the xz
  derivatives' area, floored by a quarter of the long axis), two levels blended, so far away the glints converge into the
  smooth lobe (and stop at lambda 16: no hashes). No frame index: a still view is a still pattern under TAA / DLSS.
  X's mean is 1 at every distance (the strength multiplies the lobe, never X's contrast: a clamped contrast gain made
  the near snow ~gain x brighter than the far). Not on movers, leaves and the voxel tier; only on sunlit pixels
  (`g_sunShadowFirst`). The bark's glints and its relief tap anchor at its position before the wind's sway.
* **Bright cavities** ("Bright cavities", lockable): the ambient's occlusion on snow takes the multi-bounce fit per
  channel (a cubic in o with terms linear in the albedo, max(o, ...)): a snow hollow is lit by its walls (red lost first:
  blue-white), but bounded - a rock or a wall next to the snow still shades it (the infinite-bounce form o / (1 - a (1 -
  o)) erased every contact shade at a = 0.98).
* **The tracks** (the tessellated ground; `trackShade`, taken in `terrainTessPixelNormal` before the splat and packed with
  the relief's strength in one register): "Cavity bounces (per 10 cm)" darkens the albedo to a^(1 + n) with n = bounces x
  the hole's depth (blue in a deep track); the pressed snow (full from 5 cm) has 0.7 fewer glints and 0.3 lower alpha; the
  floor's OWN SUN SHADOW ("Track sun shadow", lockable; `snowTrackSunShadow`: 4 steps toward the sun through the drawn
  deformation, 3 .. 30 cm, faded with the geometry - the shadow map and the TLAS hold no lift), on g_sunVisMaterial after
  the splat (POM sets it); "Wall glow" adds the sun through the thin wall, reddened by exp(-sigma_tr x path) (blue light)
  and x the sun's cosine on the floor's base, as g_litExtraLight after the far grass's (the path packed as p / (p + 0.5):
  no saturation at a low sun).
* **The slope rule** thins the pack's DEPTH everywhere (the ground, the rocks, the object layer, the caps): cover =
  smoothstep of depth x holds, so a cap's rim matches the roof's layer on a steep roof.
* **Melt into wetness**: the terrain's own wet look lies UNDER the snow (x 1 - the share: the ground, the rocks, and the
  water film - no pools on the pack); wet snow mirrors the sky like wet ground ("Wet snow reflection" x the meltwater, the
  terrain's sky reflection, with or without the wetness map; not on rocks, roofs and caps); the wetness CS adds
  "Melt wetting (1/s)" x the pack state's meltwater at each texel (`u_snow_meltWet`), read even when the pack is gone, so
  the ground stays wet as the snow melts out.
* **Not covered**: glints and the radiative-transfer lobe from the local lights (night snow is Lambert + GGX); the track
  shading past the tessellation range and on the caps and objects (the bright cavities carry the blue there); the snow
  BRDF and the pack's state on the far-tree volume (it takes the fresh albedo, Lambert); the 10 m tile repeat of the relief (no second, rotated tap); the relief's height keeps only ~30 %
  of the 8-bit range (`HEIGHT_SCALE` is applied in the bake, before the BC5 quantization: ~77 levels over a few cm).
