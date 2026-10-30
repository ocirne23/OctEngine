# Procedural

> Library documentation for `Code/Procedural`.
> Read [`.claude/CLAUDE.md`](../../../.claude/CLAUDE.md) first — rules, building, style, dependency direction.

The procedural world layer: diffusion terrain, FFT ocean, trees, rocks, ground clutter, terrain physics, and the
camera-centered bakes they share. Links RendererVK, File, Spatial and Physics (+ onnxruntime, zstd
PRIVATE).

## The documentation files

This file holds the globals, parking, the sampler interface, the terrain collider and the mesh cache. **Read the topic
file of every system you touch BEFORE changing it.**

| File | Covers |
|---|---|
| [Generator.md](Generator.md) | `TerrainGenV3` (the diffusion generator), world origin, `TerrainPreview`, generated bounds + the cache-only state, `TerrainSeeder`, world scale, sub-pixel detail |
| [Rivers.md](Rivers.md) | Rivers: the coarse network (`:RiverNetwork`), the units (`:RiverUnits`, `:RiverSystem`), the sampler (`:RiverTerrain`) |
| [TerrainStreamer.md](TerrainStreamer.md) | `TerrainStreamer` (node memory, edge stitching, THE world datum, the terrain-data map) and `HeightMapBaker` (`applyWaterReach`, `applyFlowField`) |
| [Ocean.md](Ocean.md) | `OceanGenerator`: world scale, the geometry clipmap, sectors, what runs where, `sampleWaterHeight`, wind, near-field shading |
| [Trees.md](Trees.md) | Trees: the piece-library model, the `.tree` grammar, piece LODs, far representations, textures, bark, leaves, baked variants, GPU expansion, world records, world mode, the preview |
| [Rocks.md](Rocks.md) | Rocks: the SDF + LOD chain model, the `.rock` grammar, DEAD WOOD, ground placement, the preview |
| [Clutter.md](Clutter.md) | Ground clutter: the `.clutter` grammar, the meshes |

## The six globals

| Global | Type | Per-frame call |
|---|---|---|
| `Globals::terrain` | `TerrainStreamer` | `update(renderer, camera)` |
| `Globals::terrainCollider` | `TerrainCollider` | `update(camera.position, terrain.activeClimateMaps())` |
| `Globals::ocean` | `OceanGenerator` | `update(renderer, camera, terrain.activeTerrainData(), terrain.seaLevel())` |
| `Globals::trees` | `TreeSystem` | `update(renderer, camera, terrain.activeClimateMaps())` — see "Trees" (it also runs the world records, "World records") ([Trees.md](Trees.md)) |
| `Globals::rocks` | `RockSystem` | `update(renderer, camera, terrain.activeClimateMaps())` — see "Rocks" ([Rocks.md](Rocks.md)) |
| `Globals::clutter` | `ClutterSystem` | `update(renderer, camera, terrain.activeClimateMaps())` — see "Ground clutter" ([Clutter.md](Clutter.md)) (after the trees: it reads their records) |

main.cpp updates them **in that order**, after the world update and before present
([main.cpp:323](../../../Code/App/main.cpp#L323)), with `terrain.render(renderer, ocean)` between the ocean and the
trees and `trees.kickWorldUpdate()` after the clutter.

All six sit in init_seg `OC_SEG_PROCEDURAL`, **the first globals to destruct** — their dtors free
renderer residency and collider bodies and may wait on in-flight jobs. **Order among them is undefined
and deliberately independent.**

`ocean.swellTravelAngle()` → `terrain.setFlowWindAngle` runs once at init and every frame before `terrain.update`, so a
"Sky/Wind" change reaches the next bake.

**Every tweak VALUE lives in the Settings library**, not in these classes: `Globals::settings.terrain` /
`terrainCollider` (Settings.Terrain, which also holds `WaterReach` / `FlowField`), `.ocean` (Settings.Ocean),
`.trees` / `.treeWorld` (Settings.Trees, with `TREE_GROVE_TYPES`), `.rockSystem` (Settings.Rocks, with
`RockWorldDesc`), `.clutter` (Settings.Clutter). Each system holds one `m_settings` reference and reads it directly;
its `initialize()` only attaches the reactions (`Tweak::onChange`: config-dirty, grid-dirty, fade bands, remesh), so
the settings must be registered first. Button settings (`reload`, `respawn`, `logStats`, ...) are cleared by the
system through that reference. `FlowField::windAngle` is NOT a tweak: `setFlowWindAngle` writes it.

> **Terrain is DISABLED by default** (`TerrainSettings::enabled = false`) — it needs 2.28 GB of ONNX weights on disk.

## Disabled = PARKED

Each system's `update()` self-gates on its Enabled tweak — the collider also on
`maps == nullptr` — with an idle latch. (The clutter keeps its types and meshes while disabled, so an enable draws at
once; only its floor map goes.)

1. The enable → disable transition clears ONCE: residents, tiles and sectors freed, requests starved,
   renderer state pushed once (terrain params 0 / ocean params `enabled=false` / cleared terrain-data
   map).
2. It then keeps **DRAINING what is still in flight** — late pump results, the terrain-data bake, the
   collider build — **scope-free** until nothing is left.
3. After that a parked `update()` is a branch and a return: **no "Terrain" / "Terrain collider" /
   "Ocean" / "Clutter" profile scopes, no renderer calls.**

A config-dirty tweak while parked (sea level, say) re-runs the terrain drain once. The ocean frees its
clipmap sectors on disable, and `rebuildGrid()` restores them on re-enable.

---

# `ITerrainSampler` — the terrain field interface

[TerrainSampler.ixx:123](../../../Code/Procedural/Private/TerrainSampler.ixx#L123).

**A PURE interface: nothing here has a default.** A default that composes the single-field samplers
point by point is only free if the field is a cheap function of (x, z) — **a tile-based generator would
silently inherit a cache lock per texel of every bake.**

All methods are **pure functions of world position — seeded, thread-safe, world-continuous**, which is
what keeps chunks, LODs, bakes and buoyancy consistent with each other. Consumers hold
`shared_ptr<const ITerrainSampler>`, **so workers finish against old maps across rebuilds.**

* **`samplePoint`** fills a `TerrainPoint` — height, waterLevel, altitude, temperature (°C), humidity,
  fogThickness, fogFalloffMul, `temperatureSeaLevel`, flowAngle01, and the inland water (`inlandWater`, `river`,
  `riverSpeed`, `waterKind`, `dryBed` - "Rivers: the sampler" ([Rivers.md](Rivers.md)); `waterLevel` is always the SEA's) — **from ONE evaluation.** Sampling
  these one at a time is fine for a noise field but not for a generator with a per-point cost.
* **`sampleRiverGrid`** fills the river influence ALONE on a grid (no terrain): placement asks for it at a resolution
  finer than its terrain grid, because channels are narrower than that. A generator with no rivers writes 0.
  RiverTerrain's goes PER PIECE: each river piece writes the points within its floodplain edge, where its influence
  ends (the max wins; NOT its carve reach, which adds the valley wall's kilometre), so the cost follows the river length
  in the rect, not its area (a per-point search of the pieces was half of TreeWorld's
  `placeChunk`). Each LAKE run writes too (not pans): 1 out to 1 px from a wet pixel (the drawn sheet reaches a pixel
  past its runs), fading to 0 over "Lake shore (px)", so "Vegetation clear" keeps trees out of the lakes' edges.
* **`sampleGrid`** is where an implementer hoists per-point overhead out of the loop, and **wide-area
  consumers should always prefer it.** For V3 it is the difference between a shared-cache lock per
  point and one per tile: *a terrain-data cascade is 512² texels resolving to a couple of dozen tiles,
  so a point-at-a-time loop meant a quarter-million lock/unlock pairs contending with the mesh
  worker.*
* Single-field accessors exist too (`sampleHeight`, `sampleWaterHeight`, `sampleHeightAndWater`,
  `sampleTemperature`, `sampleHumidity`, `sampleFogThickness`, `sampleFogHeightFalloff`,
  `sampleFlowAngle01`, `sampleAltitude`, `seaLevel`).
* **`sampleAltitude` is the MACRO elevation before fine detail** — what separates "a mountain" (height
  far above the local altitude) from "high-altitude flatland".

## `ESampleDetail`

`Full` = the real field, whatever it costs (geometry, near-field bakes). **`Coarse` = a cheap
low-resolution approximation** for bakes spanning tens of km at low texel density.

> This exists because V3 generates real tiles at real cost: **a far cascade covering the whole view
> distance would force thousands of full-detail tiles — for terrain largely beyond the mesh ring — and
> evict the ones the geometry needs.** V3 answers Coarse from its coarse stage alone, where one tile
> covers several hundred km, and the shader crossfades near → far so the fidelity drop blends rather
> than seams.

## One lapse rate for the whole world

`lapseRatePerMetre()` is ONE value, not a field. `sampleTemperature` and
`TerrainPoint::temperatureSeaLevel` are consistent with it by construction, so a consumer can
re-derive the temperature at any height from the baseline alone.

> **Why the map bakes the sea-level BASELINE, not the temperature.** The terrain-data map's two
> cascades bake DIFFERENT heights for the same spot — the near one the full-detail surface, the far one
> its multi-km average, which cannot know a peak exists. **A baked temperature is only valid at the
> height it was baked from** (a baked sample put peaks ~10 °C too warm in the far cascade, and the
> texture flipped at the crossfade). A baseline plus one shared constant has no such anchor: every
> consumer evaluates at the height it shades, and the cascades agree by construction.

A generator that models no lapse reports the baseline equal to the temperature, so the two agree
either way.

## Climate encoding

Shared with the shaders (`terrain_height.inc.glsl` mirrors them — **keep them in step**):

| Constant | Value |
|---|---|
| `TEMPERATURE_MIN_C` / `MAX_C` | −25 / 50 °C, 8-bit (~0.29 °C steps) |
| `HUMIDITY_FULL_PRECIP_MM` | 2200 mm/yr = humidity 1.0 |
| `FOG_FALLOFF_MUL_MAX` | 4.0 |

`temperatureTo01` / `precipTo01` map into the normalized space **the tree / rock / clutter Placement climate boxes
and the terrain shader's texture splatting BOTH share.**

`fogFalloffFromTemperature` — cold air hugs the ground, warm air lets fog tower. **It is not baked**: it is a
pure function of temperature, so the shaders recompute it. The lapse rate is not baked either — it is one value, pushed
as `u_terrain_lapseRate` (`terrainTemperatureAt` in `terrain_height.inc.glsl`). The packed channel's four bytes are fog
thickness | flow direction | sea-level temperature | humidity (see "The terrain-data map" in
[TerrainStreamer.md](TerrainStreamer.md)).

---

# `TerrainCollider`

"Terrain/Collision" tweaks. A **focus-centered** ring of small static triangle-mesh collider tiles.

**Sampled from the SAME `ITerrainSampler` the terrain renders**, on a world lattice of "Spacing" with the
render mesh's triangulation, **so bodies rest on the drawn surface.** It is exact only when "Spacing" equals the render
LOD0 spacing (`chunkSize / lod0Res`: 2 m at the defaults; "Spacing" defaults to 1 m).

It **never touches the render nodes**: a collider only needs the ground near dynamic bodies. Tiles are "Tile size"
(32 m) and exist only within "Radius" of the focus
(the camera — thrown bodies start there).

Each tile is `sampleGrid`'d and BVH-built on **ONE in-flight job, nearest-first** (`"Terrain collider
build"`, Low). `createCollisionMesh` is standalone and thread-safe, **which is what lets the build run
off-main at all.**

**The per-frame work is a job too** (`"Terrain collider"`, Normal, `m_updateCounter`): `update` on main
only snapshots the inputs (maps, focus, the clamped tweaks) and kicks it. The job drains the finished
build, creates / destroys the static bodies (layer "Terrain"; box3d create/destroy serialize on
`g_bodyLifecycleMutex`), scans for the nearest missing tile and kicks its build. **main.cpp joins it
(`joinUpdate`) right before `kickPostUpdateJobs`**: the Sim batch (the game's nav feed) reads body
positions, and the next frame's main-thread physics users (player controls, scripts, net receive, the
step) must not overlap a body create/destroy. Between the kick and the join main runs only the ocean, the terrain
render kick, trees / rocks / clutter, particles, force and the UI kick — none touches box3d. Sub-scopes `"Terrain collider clear"` /
`"... create body"` / `"... evict"` show the box3d cost.

Tiles clear and rebuild on a sampler identity change, exactly like the streamer's residents; eviction
has half-tile hysteresis. `maps == nullptr` clears everything (on main, once, after joining the update
job). The dtor waits out the update job, then the in-flight build (main helps).

Verify with `Physics/Debug/Draw colliders`.

---

# The generated-mesh cache (`:MeshCache`)

The tree, rock and clutter generators' OUTPUT is saved under `Assets/Local` and loaded instead of generating again:

| What | File | Key (hashed into the file's header) |
|---|---|---|
| A tree species: its piece library, its baked variants, their far-volume density grids | `Local/Trees/Meshes/<species>.treemesh` | the `.tree` file's text, `TREE_DENSITY_RES` |
| A rock variant: its `RockShape`, LOD meshes, density grid | `Local/Rocks/Meshes/<type>_v<i>.rockmesh` | the `.rock` file's text, the grid resolution, the variant seed |
| A clutter type's meshes (every LOD) | `Local/Clutter/Meshes/<type>.cluttermesh` | the `.clutter` file's text, `Clutter/Mesh resolution` |
| A tree material's textures AS UPLOADED (bark, leaves, each billboard; `CachedTexture`) | `Local/Trees/Textures/<name>.tex` | the `.tree` file's text, `Trees/Compress textures`, the size |
| A dead-wood rock type's tinted bark, as uploaded | `Local/Rocks/Textures/<type>.tex` | the bark `.tree` file's text, the tint, the size |
| A flower clutter type's texture (BC3), as uploaded | `Local/Clutter/Textures/<type>.tex` | the `.clutter` file's text |

* **One file per unit, the key INSIDE**: a changed input is a miss that regenerates and overwrites the file - no stale
  files pile up. A damaged or truncated file is a miss too. Format: a header (magic, kind, version, hash, raw size),
  then the zstd payload (level 3; textures 9: written once, a few MB); vectors of trivially copyable types are written raw.
* **The key does not cover the generator CODE.** A change that moves a generator's output for the same inputs must
  bump `TREE_MESH_CACHE_VERSION` / `ROCK_MESH_CACHE_VERSION` / `CLUTTER_MESH_CACHE_VERSION` / `TEXTURE_CACHE_VERSION`
  (MeshCache.ixx; the textures also cover the mip builders and the BC encoders), or the folder must be deleted. The clutter's pebbles come from `generateRockVariant` too: a rock generator change
  bumps both.
* Trees load on main (`TreeSystem::reload`, under its `AllowMainThreadIO`); the rocks and the clutter load / save in
  their generation jobs. A cache hit also makes Debug and the optimized builds draw the same floats (the generators'
  `/fp:fast` last bits).

# Build note

`Procedural`'s diffusion `.cpp` TUs override the global flags to `/fp:precise /wd5050` — **load-bearing;
see `Code/Procedural/CMakeLists.txt`.**
