# Procedural

> Library documentation for `Code/Procedural`.
> Read [`.claude/CLAUDE.md`](../../.claude/CLAUDE.md) first — rules, building, style, dependency direction.

The procedural world layer: diffusion terrain, FFT ocean, trees, rocks, ground clutter, terrain physics, and the
camera-centered bakes they share. Links RendererVK, File, Spatial and Physics (+ onnxruntime, zstd
PRIVATE).

## The six globals

| Global | Type | Per-frame call |
|---|---|---|
| `Globals::terrain` | `TerrainStreamer` | `update(renderer, camera)` |
| `Globals::terrainCollider` | `TerrainCollider` | `update(camera.position, terrain.activeClimateMaps())` |
| `Globals::ocean` | `OceanGenerator` | `update(renderer, camera, terrain.activeTerrainData(), terrain.seaLevel())` |
| `Globals::trees` | `TreeSystem` | `update(renderer, camera, terrain.activeClimateMaps())` — see "Trees" (it also runs the world records, "World records") |
| `Globals::rocks` | `RockSystem` | `update(renderer, camera, terrain.activeClimateMaps())` — see "Rocks" |
| `Globals::clutter` | `ClutterSystem` | `update(renderer, camera, terrain.activeClimateMaps())` — see "Ground clutter" (after the trees: it reads their records) |

main.cpp updates them **in that order**, after entity updates and before present
([main.cpp:803](../App/main.cpp#L803)).

All six sit in init_seg `OC_SEG_PROCEDURAL`, **the first globals to destruct** — their dtors free
renderer residency and collider bodies and may wait on in-flight jobs. **Order among them is undefined
and deliberately independent.**

`ocean.swellTravelAngle()` → `terrain.setFlowWindAngle` is wired once at init.

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

[TerrainSampler.ixx:106](Private/TerrainSampler.ixx#L106).

**A PURE interface: nothing here has a default.** The defaults it used to carry composed the
single-field samplers point by point, which is only free if the field is a cheap function of (x, z) —
true of the old noise generator, **false of a tile-based one, which would silently inherit a cache
lock per texel of every bake.**

All methods are **pure functions of world position — seeded, thread-safe, world-continuous**, which is
what keeps chunks, LODs, bakes and buoyancy consistent with each other. Consumers hold
`shared_ptr<const ITerrainSampler>`, **so workers finish against old maps across rebuilds.**

* **`samplePoint`** fills a `TerrainPoint` — height, waterLevel, altitude, temperature (°C), humidity,
  fogThickness, fogFalloffMul, `temperatureSeaLevel`, flowAngle01 — **from ONE evaluation.** Sampling
  these one at a time is fine for a noise field but not for a generator with a per-point cost.
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

> **Why the BASELINE is what the map bakes, not the temperature.** The terrain-data map has two
> cascades, and they bake DIFFERENT heights for the same spot — the near one the full-detail surface,
> the far one its multi-km average, which cannot know a peak exists. **A baked temperature is only
> valid at the height it was baked from**, so the far cascade reported the temperature of the plateau a
> peak stands on — 10.6 °C too warm, wider than a climate box, and its texture flipped at the
> crossfade. A baseline plus a shared constant has no such anchor: every consumer evaluates at the
> height it shades, and the cascades agree by construction.

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

`fogFalloffFromTemperature` — cold air hugs the ground, warm air lets fog tower. **It is no longer
baked**, being a pure function of temperature, so the shaders recompute it and **its 8 bits in the
packed channel now carry the LAPSE RATE**, which nothing else can reconstruct.

---

# `TerrainGenV3` — the diffusion generator

`:GeneratorV3`, `Private/Diffusion/`. An ONNX Runtime + DirectML pipeline behind `ITerrainSampler`.

**Why the class looks the way it does:** an `ITerrainSampler` looks like a pure function of (x, z),
but this one generates **256×256-pixel TILES through three ONNX models.**

* **Sampling a point means bilinearly reading a resident tile.** A miss BLOCKS the caller while the
  tile is generated — **~1.5 s cold, ~0 once resident**, and one tile covers ~225 chunks at chunkSize
  512.
* **All inference is serialized onto one thread.** *Required, not a convenience*: the tile store is not
  thread-safe, and `sampleHeight` is called from the terrain streamer worker, the tree record pumps AND
  the height-map baker. The lock is a fiber-parking `JobMutex`
  ([GeneratorV3.cpp:606](Private/Diffusion/GeneratorV3.cpp#L606)) so a ~1.5 s cold-tile wait parks a
  FIBER rather than a pooled worker, and concurrent requesters park on a per-tile `JobEvent`.
  **The lock covers ONLY the pipeline call** (`get` / `getCoarseSlice`, plus reading the seed and
  precision it ran with): the plane assembly, the sea-level baseline recovery, the coarse lapse
  regression and the quantised zstd disk write are the tile's own CPU work and run UNLOCKED, so the
  next tile's dispatches start while they run instead of the GPU idling through every tile's tail.
  Concurrent callers (the streamer's pumps, the seeder) are what make that overlap happen.
* **The 2.28 GB of models load once per process and are RESEEDED, never reloaded.** So constructing a
  `TerrainGenV3` is cheap and a tweak rebuild costs nothing unless the seed actually moved. The load is
  DEFERRED until "Terrain/Enabled" — `rebuildMaps` early-outs disabled without constructing the
  generator, **and constructing it is the only load kick. There is no unload API**: disabling after a
  load keeps the models resident.
* **`isReady()` is a real state.** Until the models are downloaded and loaded it is false and the
  generator must not build geometry — **it would bake a flat world into the chunk cache.**
  TerrainStreamer polls it and rebuilds once it flips.

Tiles cache to disk under `Assets/Local/Diffusion/<seed>/` (zstd).

## World origin

`TerrainConfigV3::originX/Z` (the streamer's `Terrain/Origin X (m)` / `Origin Z (m)` tweaks): where
in the seed's model-space world the engine's (0, 0) sits. **Applied at the ONE world → lattice mapping
every sample goes through** (`TileBlock::latticeX/Z`), so point queries and grid fills place the world
identically; the detail and climate noise stay in engine coordinates on purpose (seeded noise, not
model data). The tile caches are keyed in model space and never see it, so moving the origin
regenerates chunks and bakes but no tile already on disk. The lobby's "Seed world" pick sets it
through overrides.

## `TerrainPreview` — the lobby's world overview

`:TerrainPreview`, owned by the App's `Session` (a stack local in main), not a global. `request(seed,
mpp)` constructs a `TerrainGenV3` for the seed (which kicks the model load and RESEEDS the shared
runtime — **so the session disables a live world of another seed first**) and, once `isReady()`, runs
ONE Low job that `sampleGrid`s a **256² grid at `ESampleDetail::Coarse`, one texel per coarse pixel**
(`nativePerCoarsePixel() × metersPerPixel` world metres — 33 km across at the clamped 0.5 m/px, 1,970 km
at 30; the page scales it to 512 px), in 32-row bands so a cancel lands within one band's tile fetches, then colours it (sea by
depth, land by humidity / altitude / temperature, a hillshade, a one-texel coastline) into an
immutable RGBA8 `Image` shared by pointer. `worldOffsetAt(u, v)` maps a pick on the image to the
origin offset that puts the spot at (0, 0).

## Generated bounds and the cache-only state

`TerrainConfigV3::bounded` + `boundsMin/Max` (engine metres; the streamer's `setGeneratedBounds`, set
by the session to the seeder's tile-aligned coverage): **inside a bounded generator a FULL tile is
only fetched inside the rect** — `resolveBlock` leaves the slot null past it — **and every sample that
lands on a null slot falls back to the COARSE stage** (`sampleField`, and `sampleGrid` resolves the
coarse block once per grid). So the terrain-data bake's 4 km Full near cascade, the collider and the
tree records cost nothing past the playable area, and the streamer's ring scan skips chunks that do not
touch it. Without bounds a 512 m area still pulled ~800 tiles: the 3×3 chunk ring (~600 at 128 m
tiles) plus the near cascade (~1000).

`TerrainGenV3::unloadModels()` drops the ONNX sessions and enters the **cache-only** state: `isReady()`
(= "sampling is valid") stays true, `modelsLoaded()` (= "inference is possible") is false, a cache
miss is a null tile (coarse fallback, else sea level), and the next `TerrainGenV3` construction
reloads — through the normal not-ready → ready handover. The session unloads at a seeded match's
Start.

**"Terrain/V3/Load models"** (on by default) is the same state as a SWITCH:
`TerrainGenV3::setModelLoadingEnabled`, applied at the top of `rebuildMaps` before any construction.
Off: models already up are unloaded, and `beginLoad` enters cache-only instead of starting the loader,
so the terrain runs entirely on the `.tile` files already under `Local/Diffusion/<seed>/`. `setPrecision`
in cache-only only switches the cache folder (and clears the RAM caches) — it never reloads. The
preview and the seeder gate on `canGenerate()`: the models are up, or loading is off and the caches
serve (a preview then shows only coarse tiles generated before). **A tile missing from disk is retried
from disk on every fetch** — cheap per call, but a camera flying far past the cached area pays one
failed read per tile per grid.

## `TerrainSeeder` — pre-generating the playable area's tiles

Same partition and ownership. `start(seed, mpp, origin, halfSize)` constructs a `TerrainGenV3` with
that origin and, once ready, runs ONE Low job over the full-detail tiles covering
`[-half, half]²` in engine space (`fullTileRange`), **nearest the origin first**: each
`prefetchFullTile` is a disk read when the tile is cached under `Local/Diffusion/<seed>/` and ~1.5 s
of inference otherwise, so a previously seeded area comes back in seconds. It only makes the TILES
exist — the mesh, the collider and the bakes stay the streamer's live work — and reports done /
total / cached plus the tile-aligned model-space rect it covers (`coverage`, the preview map's
square). The generator's `fullTileRange` / `fullTileWorldRect` / `isFullTileCached` /
`prefetchFullTile` exist for it. `TerrainStreamer::streamStatus()` remains the "how far has the mesh
ring streamed in" read.

## World scale

**`metersPerPixel` is a UNIFORM scale**: it shrinks elevation and detail by the same factor as the
horizontal, **so a compressed world keeps the model's proportions** — same slopes, shapes and climate,
just smaller and quicker to fly across.

| Value | Meaning |
|---|---|
| **30** | The model's true training scale; continents are continent-sized and peaks ~10 km. A tile is then 7.68 km. |
| 3 | A 10× compressed world; a tile is 768 m. |
| **0.3 (the default)** | A 100× compressed world; a tile is 76.8 m. |

**Lowering it is quadratically more expensive** — the same view distance spans more model pixels, so
more tiles must be generated. `heightScale` is a pure vertical exaggeration ON TOP (1 = real
proportions).

## Sub-pixel detail

The model resolves 30 m/px, so the diffusion field is smooth below that and a close-up hillside would
be bilinear mush.

A **slope-masked** noise layer rides on top (`:Noise` `fbmEroded`), in two bands — coarse crags
(wavelength 220 m, amplitude 38, 4 octaves) and fine break-up (45 m / 11 / 3 octaves) — with
`detailSlopeGain` 0.75 as `sf = min(1, slope * gain)`.

> **Plains and seabed stay smooth; only mountain faces get crags.** Wavelengths are in MODEL metres and
> ride `metersPerPixel`, so **the OCTAVE counts are what set how far below the model's 30 m/px the
> terrain actually has anything in it** — that is the dial for "detailed at full world scale".

This is the `sampleAltitude` (macro) vs `sampleHeight` (macro + detail) split.

---

# `TerrainStreamer`

"Terrain*" tweaks. Render-only chunk streaming.

* A bounded LOD ring around the camera: `ringRadius` 96 chunks (24 km), `chunkSize` 256, `lod0Res` 128, `maxLod`
  4, `lodStep` 1.2 chunks for the LOD0 band (each next band twice as wide, geometric), `fullResDist` 1.2,
  `skirtDepth` 5, `maxUploadsPerFrame` 16. (Until 2026-10-03: 1024 m chunks, `lod0Res` 512, ring 32, `lodStep` /
  `fullResDist` 0.3 - the same 2 m LOD0 texel and LOD band metres; the ring stays below the old 32 km because 128
  chunks would be ~51 k chunk meshes against the 16-bit mesh index.)
* Generated on up to "Gen jobs" **self-continuing Low-priority pump jobs** (`kickPump` CAS-claim +
  exit-recheck protocol, nearest-first). **The upload layout is built IN the pump**
  (`RenderMeshData::build`, pure) and `Renderer::createMesh` runs on the upload job (below),
  **so warm-tile mesh builds overlap cold V3 waits, which park fibers.**
* **Pre-emption points** (see Threading): the pump yields to higher-priority jobs between chunks,
  `generateChunk` after its `sampleGrid` and per vertex row, and `TerrainGenV3::sampleGrid` per row.
  **All of them sit OUTSIDE the V3 pipeline `JobMutex`** — the Normal collider job samples terrain
  and would park on that lock behind the very job holding it, so a point under the lock deadlocks.
  A cold-tile inference itself is never interrupted; it parks the fiber anyway.
* **A chunk is ONE `RenderMesh` + its `RenderNode`, no `ObjectContainer`** (see RendererVK "Single
  meshes"): no per-chunk material (every chunk shares `m_material`, `createMeshMaterial(TerrainLit)`),
  names or node tables, and no LOD chain (the ring IS the LOD). A `Resident` declares its `mesh`
  FIRST so it is destroyed AFTER the `node` that draws it; a chunk leaving the ring frees its mega-buffer
  ranges and MeshInfo slot, **so residency stays bounded across a session.**
* Each resident registers a `SpatialEntry` on `SpatialLayer_Terrain` with **`spawnVisible = false`** —
  chunks stream in off-screen constantly, and the guard would pin each one in the main pass until it
  first entered the frustum. **Its userData is the `Resident*`**, so the hand-over push needs no
  lookup and reaches the chunk's node AND its vegetation. Its sphere is the node's bounds grown for the
  vegetation (a plant up to 64 m tall, its crown up to 16 m past the chunk's edge). That address must
  outlive every list that holds it: residents are heap-held
  (`m_residents` maps to a `unique_ptr`), and an evicted / cleared one is **retired**
  (`retireResident`: culling entry, node and mesh released at once; the memory kept in `m_retired`
  until `visibleCollectGeneration` moves past its value, freed at the top of `update`). The walk
  meets its destroyed node and `renderNode` skips it.
* **The push is `render(renderer, ocean)`, not `update`.** main.cpp calls it right after
  `ocean.update` (which may rebuild the sector grid). It kicks `ocean.render(gate)` first, then its
  own job, whose ONE walk of the hand-over pushes every value: a tagged ocean sector's `RenderNode*`
  or a chunk's `Resident*` (its node, and its vegetation - below), each with its node's own pass mask
  (chunks `PASS_ALL`, sectors `PASS_MAIN` or 0 when dry). Nothing is resolved on main. It runs with the terrain disabled too (the walk still
  pushes the ocean's sectors; `m_renderReady` = "update ran enabled" gates the chunk-only parts).
* **The render push never walks the ring** (thousands of residents, a handful on screen). Two sets:
  1. **Main-visible chunks** come from the cull job's Main stamp through the Spatial visible-set
     hand-over, collect slot 1 (`setVisibleCollect(SpatialLayer_Terrain, 1, ECollect::UserData)` in
     `initialize`, `visibleUserData(1)` in `render`) → `PASS_ALL`. The slot holds userData VALUES
     read in the cull job (zeros dropped there - the removed scatter's groups were the only ones), so the walk reads no pool
     row; a chunk evicted since the stamp is a retired, destroyed node, skipped by the push.
  2. **Main-culled chunks keep shadow + GI** (the ground behind the camera must stay in the TLAS and
     the sun cascades), but only inside a `forEachInSphere` around the renderer's scene focus with
     radius `max(shadow maxDistance + casterPad, RT/TLAS Range)` — the range past which the GPU
     shadow cull and the TLAS range bound drop the push anyway; farther ground gets its sun shadow
     from the terrain march over the baked height map. `MainOnly` skips this set; a main-stamped hit
     is skipped by the query itself (`forEachInSphere(..., SpatialPassBit_Main)`), tagged sectors and
     zero entries by the emit. Culling `Off` keeps the plain walk over every resident.

  **The walk and the pushes run on workers** (`"terrainRenderPush"`, High, `m_renderCounter`); main.cpp calls `joinRender()` right before `present`, and `update`
  / `clearResidents` join it before they touch `m_residents`. The job fans the hand-over list out over a High
  `parallelFor` (`"terrainRenderPushChunk"`, auto-grain `m_renderPushCost`) and runs the sphere query BESIDE it as a
  job of its own (`"terrainRenderPushSphere"`), then merges the vegetation. **The two sets are disjoint** (the
  query skips main-stamped entries, and the list holds each entry once), which `renderNode` needs: it writes
  the node's transform upload state. Culling `Off`'s walk over `m_residents` stays serial (a debug mode).
* **THE VEGETATION IS STORED IN THE CHUNKS** (`setVegetation(lookup, sink, numChunks)`, TreeSystem's GPU
  tree set): every resident holds the index of its coordinate's vegetation chunk (`lookup`, -1 = none;
  re-stamped on every resident by `setVegetation`, which joins the walk first). The walk notes it with the
  chunk's pass mask (main-stamped `PASS_ALL`, the shadow/GI sphere `PASS_SHADOW | PASS_GI`), merges a
  coordinate drawn twice (a LOD hand-over's old + new resident: the masks OR'ed), and hands the frame's
  list to `sink` at its end, on the walk's worker. The pushes note it from several workers: each mask is
  OR'ed through an `atomic_ref`, and the push that first sets a chunk's mask lists it in `m_vegTouched`
  (one slot per vegetation chunk, `m_vegTouchedCount`); the list order does not matter to the tree culls.
  `vegetationRouted()` = this frame's walk carries it
  (the chunks draw and a sink is set); otherwise the owner submits its vegetation itself.
  **`restampVegetation(coord)`** (TreeSystem's world mode adds / removes one coordinate): re-stamps that coordinate's
  residents (every LOD; retired ones to -1) from `lookup` WITHOUT joining the walk - the stamp is an atomic, the walk
  sees the old or the new index, and the owner keeps a removed index unused until the next frame.
* **THE GRASS STANDS ON THE CHUNK MESHES** (RendererVK "Procedural grass"): at the end of every enabled `update`, the
  residents of the columns within `renderer.grassRange()` (a few `m_residents` lookups per LOD, no walk) go to
  `Renderer::setGrassGround` (coord, first vertex in the mega-buffer, grid cells per side); the GPU reads their vertices.
  The renderer keeps the list ONE frame, so a disabled terrain simply sends none.
* **Eviction does not walk the ring either.** The unwanted residents (column outside the ring, or
  wanting another LOD) are a function of the ring and the resident set only, so `m_evictCandidates`
  is rebuilt by one walk when `ringMoved` or a chunk uploaded; every frame checks only the candidates
  against the stamps for the hole-free handover. **The walk is a job** (`"terrainEvictScan"`, Normal,
  `m_evictScanCounter`), kicked with the ring scan at the end of `update` over the same snapshot
  (`m_ringScanIn`) and swapped in after the join at the top of the next one. Its list is one ring old,
  so the per-frame check judges each candidate by the CURRENT ring (a candidate wanted again is dropped).
  The check and the retire stay on main: `getPassMask` would race the spatial pool growth and the cull
  job's stamps in that window, and an unregister is not allowed inside the cull job's kick/join window.
* **The ring scan is a job too** (`"terrainRingScan"`, Normal, `m_ringScanCounter`), kicked LAST in
  `update` — after the drain and the eviction, the frame's last writers of `m_residents` /
  `m_pending`, which the scan only reads — from a by-value snapshot (`m_ringScanIn`; the job
  captures `this` only, inline job storage is small). The NEXT `update` joins it first and applies
  `m_ringScanOut`: keys that became pending or resident meanwhile are skipped, the rest go pending,
  get published and kick the pump. One frame of request latency against seconds of generation; a
  request the ring moved away from is stale like any other and dropped by the pump.

* **The mesh upload is a job too** (`"terrainUpload"`, Normal, `m_uploadCounter`).
  `Renderer::createMesh` copies the chunk into the staging ring. A LOD0 chunk is ~15 MB, a wrap of the
  100 MB ring waits a staging fence, and the frame's first shared-buffer write drains the GPU. So 16
  uploads on main were ~45 ms. `update` only PICKS the batch into `m_uploads` (cap "Uploads/frame"
  AND "Upload MB/frame", 48 MB default; the first chunk always goes; the picks stay in `m_pending`).
  **main.cpp kicks it right AFTER `present`** (`kickUploads`), so it runs through the frame-pacing
  wait, and **joins it before the next frame's "Frame kicks"** (`joinUploads`): `createMesh` grows the
  MeshInfo / per-mesh tables that the begin-frame job and the entity pass's `renderNode` read.
  Everything it calls is thread-safe (the MeshDataManager alloc mutex, the staging mutex, the renderer
  `m_spawnMutex`), like parallel entity spawning. **The batch fans out across workers**: the job uploads
  the FIRST chunk alone — the frame's first shared-buffer write drains the GPU under the staging mutex,
  and a fan-out would block one worker per chunk behind it — then a grain-1 `parallelFor`
  (`"terrainUploadMesh"`) uploads the rest. That only pays because the staging memcpy runs OUTSIDE the
  staging mutex (RendererVK, `StagingManager`). The next `update` adopts the batch (`"adoptUploads"`:
  re-validate, `spawnMeshNode`, the spatial registration — all cheap, all on main). One more frame of
  latency. `clearResidents` joins it and drops the batch.

  What stays on main: the config/model polling, the terrain/texture/wet param setters, the fog
  height-map handover (the bake itself is already a job — `HeightMapBaker::update` polls it), the
  upload pick + adopt, and the candidate check + retire (the retire releases GPU residency into the
  renderer and unregisters the culling entry; the candidate walk is the `"terrainEvictScan"` job).

## It owns THE world datum

Handed out, never duplicated:

| Accessor | Consumer |
|---|---|
| `seaLevel()` | **The ocean floats on this rather than owning a second copy** — the generator builds its heights around it (moving it regenerates chunks), and the swash gate compares the two, so a fork switches the swash off everywhere. Valid even while terrain is disabled. |
| `activeClimateMaps()` | The collider, the trees, the rocks. **nullptr while terrain is disabled** — consumers treat that as "no terrain" rather than sampling a field that is not drawn. |
| `activeWaterReach()` / `activeFlowField()` | The ocean's shore bake, **so the two bakes cannot drift apart.** |
| `activeTerrainData()` | The ocean's CPU buoyancy and wind-steering votes. **A re-bake swaps in a NEW object**, so a consumer holding the old `shared_ptr` keeps a coherent snapshot. |

## The terrain-data map

Baked around the camera and shipped to the GPU through `Renderer::setFogTerrainHeightMap`.

**Two cascades, RGBA32F, cascade-major** (`BakedTerrainData`; the layout matches
`terrain_height.inc.glsl`):

| Channel | Contents |
|---|---|
| R | terrain height (world Y) |
| G | water surface level |
| B | 4×8 packed bits — flow direction in bits 8–15, **bit-cast, never float arithmetic** |
| A | macro altitude |

Consumed by fog terrain-follow, ocean depth/level (GPU through `terrain_height.inc.glsl` plus the CPU
copy), terrain colouring and the terrain sun march.

It also bakes the splat textures, PACKED into THREE per set (one texture fetch fewer per splat layer than the
four sources; since 2026-09-28): `diffr` = albedo + ROUGHNESS in the alpha (the ARM's G; BC3), `nor` (BC5),
`hao` = HEIGHT (the `disp` source's R, 0.5 = flat without one - the terrain relief's input: the parallax +
height blend in "Terrain/Textures", the displacement in "Terrain/Tessellation"; see the RendererVK CONTEXT) +
AO (the ARM's R; BC5). The ARM's metalness is dropped (terrain is never metallic). A packed output is stale
when EITHER of its sources is newer. Compressed to `Assets/Local/TerrainTex` (old `_diff` / `_arm` / `_disp`
files there are leftovers of the unpacked set) — a background job
kicked ONCE, at startup while the terrain is enabled or from `updateTerrainTextures` when it is
enabled later (`kickTexBake`; a disabled terrain never reads the source image sets); **the TERRAIN
shader falls back to flat colours until that bake finishes.** Each entry's climate box registers with
its textures (`TerrainSplatMaterial::climate`: temperature as t01, precipitation in mm/yr); only the
mm-per-full-humidity divisor stays live, pushed every frame as `TerrainTexTweaks::precipFullMm`. Each GROUND entry
also carries a `.grass` amount (0..1, `TerrainSplatMaterial::grass`): the grass grows where the splat draws that
texture, by that amount (RendererVK "Procedural grass").

The streamer also owns the **"Terrain/Water" tweaks** - ONE wetness field (rain, the ocean's swash,
submersion) driving ONE water surface: the field (texel size, update rate, diffusion, rain, dry time +
temperature sensitivity, wet-in, slope drain), the surface (fill start / full / curve = how far the wetness
fills the splat relief, the film max slope + slope fade that sink it on slopes, the film flow speed + min slope + cycle (ripples running downhill), edge fade), the hand-over to the ocean (ocean blend = the film fading into the
ocean's water, ocean edge fade = the ocean blending out at its edge over the film) and the look (waviness, normal scale, wind ripples,
water (film) / wet ground / underwater ground roughness, wet darkening, darkening / roughness thresholds, wet normal scale, glint size / coverage / roughness (sparse near-mirror patches on the wet gloss), the drying pattern = dry islands from a world fBm: strength, darkening / roughness edge, size, relief share, contrast). They push every frame from `updateTerrainTextures` through
`Renderer::setTerrainWetParams`; the clipmap itself - swash injection, decay, the toroidal window - is the
renderer's (`TerrainWetnessPipeline`; see Terrain surface water in
[`Code/RendererVK/CONTEXT.md`](../RendererVK/CONTEXT.md)).

---

# `HeightMapBaker`

The async snapshot bake driver: **ONE Low job in flight** (JobCounter-polled), re-baking on camera
drift past range/4 or a config change — **configs carry `operator==` so tweaks stay live**.

**Two passes for a NEW sampler.** The near cascade at Full detail is every full-detail tile under the
near range — at 4 km and sub-metre mpp ~1000 V3 tiles, ~1.5 s each cold, serialised on the one
inference lock — and the map ships all-or-nothing, so after a reseed the climate textures stayed flat
for tens of minutes while the mesh was long visible. A sampler the baker has not shipped yet (first
map, reseed, config rebuild) therefore gets a **quick pass first — both cascades at Coarse detail,
seconds** — then a Full-near re-bake at the same centre replaces it. Coarse and Full share the fitted
climate baseline, so the textures land with the quick pass; only the near heights refine later. A drift
or rule change on a known sampler goes straight to Full (its tiles are what the mesh streamed).
**A bake whose inputs go stale while it runs is cancelled** (an atomic checked between 16-row bands of
`sampleGrid`, so at most one band of tile fetches is wasted), and teardown cancels before it waits.

**Centres snap to the COARSEST cascade's texel lattice**, so no cascade's features ever swim as the
camera moves. Both cascades measure the same world distance at their own texel size, **so near and far
agree.**

## `applyWaterReach`

> **"Sea level everywhere is a lie the ocean believes."** A generator that models no lakes (V3)
> reports the ocean's level at EVERY point on the planet, so every scrap of terrain within a metre of
> it — an inland hollow, a river flat — reads as shoreline and the ocean runs swash up it.

**The swash already has the right gate**: it fades where the baked water level departs from sea level,
which is how landlocked water is meant to be excluded. **So the fix is not a new rule, it is telling
the truth** — a chamfer distance-to-ocean pass sinks the baked water level under any ground the ocean
cannot reach, and the existing gate does the rest. The terrain shader's beach overlay keys on the same
field, so inland sand goes with it.

* **"Can reach" is a DISTANCE question, and the bake is the only place it is cheap**: it owns the whole
  camera-centred height grid, so one chamfer pass gives every texel its distance to real ocean in world
  metres. A per-point generator query would have to search its own field per sample, and the Coarse
  level has no fine elevation to search at all.
* **It applies to SUBMERGED ground as much as dry land.** A plateau at −0.05 m is under water by
  definition, so a land-only rule could never drain one — and those are exactly what V3 produces,
  kilometres of sea-level film with no ocean in reach. The clipmap rides the baked level, so sinking it
  here **actually removes the water** rather than merely muting its swash.
* **A real shelving bay is kept wet by the RADIUS**: its apron is too shallow to seed itself, but it
  sits close to water that does. A radius shorter than the apron makes the sea retreat off it.
* **LAKES are never touched** — a lake's surface already differs from sea level.

## `applyFlowField`

The 8-bit per-texel flow direction: **toward land through the surf zone, downhill elsewhere**, eased
back into the offshore `windAngle` the app pushes in each frame.

**It runs AFTER `applyWaterReach` on purpose** — reach decides what is water at all. Baked into both
maps' flow bits, and handed out through `activeFlowField()` so the ocean's shore map bakes the SAME
directions: otherwise the wave travel direction would turn where one map hands over to the other.

---

# `OceanGenerator`

"Ocean*" tweaks. The CPU side of the FFT/Tessendorf water.

## World scale

**"Ocean/World scale" is the ocean's `metersPerPixel`**: every metre-valued ocean tweak is a MODEL
metre, and the sea is drawn at model × scale (1 = the model sea and **the default**; tuned by eye rather than tied
to the terrain's "Meters per pixel"). Applied in ONE place, `pushOceanParams`, which fills the scaled `m_params` the renderer gets —
**and the CPU buoyancy mirror reads `m_params`, never the tweak members**, so the two cannot disagree
about the scale.

| Quantity | Scaling | Why |
|---|---|---|
| fetch, depth, cascade patch sizes | × s | Froude similarity: with gravity untouched this is the ONE scaling of the JONSWAP/TMA inputs under which wavelengths AND heights both come out × s — the scaled sea is a shrunk copy, not the full-size spectrum aliased into small patches. |
| wind speed | × √s | The velocity half of the same similarity (`U²/(F g)` and the wave-age ratio stay invariant). |
| every other metre (shore depths, cull slack, RT ranges, horizon offset, steer range, bubble depth, foam texel, **ring cell**) and the foam drift speed | × s | Same world, fewer metres. The ring cell shrinking keeps the clipmap's detail per wavelength and its reach per model kilometre. |
| absorption, SSS strength (per metre) | ÷ s | The same water column in fewer metres, so deep water stays deep-coloured. |
| dimensionless ratios (amplitude, choppiness, the approach-band fraction, swash amplitude, foam thresholds) | — | The break acceleration is a fraction of g, invariant under Froude scaling. |
| sea level | — | The world datum, owned by the terrain. |
| **outside the ocean tweaks:** the "Ocean/Spray *" metres and m/s, the spray `.pfx` size / gravity / turbulence and "Fog/Caustic shore fade" | × s (spray rate per m² ÷ s², "Fog/Caustic depth fade" per m ÷ s) | The renderer reads `OceanParams::worldScale` (`Renderer::getOceanWorldScale`): the sea keeps its model PERIODS, so a speed or an acceleration scales like a length. Applied in `buildUboOcean` / `buildUboFog` and, for the emitter, in `ParticleSystem::update`. The underwater fog boundary's offset is NOT a scaled tweak: "Fog/Underwater wave offset" (a ratio) lowers it by × the deepest LIVE wave trough (`getWaveTrough`, world metres already): a higher sea needs a lower boundary, or the murk peeks through the troughs the fog's coarse froxels miss. (A fixed "Underwater offset (m)" was removed, 2026-09-30.) |

**The periods stay the model sea's.** Froude scaling alone shortens them by √s, and a miniature sea at
real-sea speed reads as racing. So `OceanParams::timeScale` = √s slows the spectrum's clock
(`ocean_spectrum.cs.glsl`, the `e^{iωt}` evolution only) back to the model periods: the sea is the model
sea in slow motion, shrunk. The breaking-crest acceleration stays in spectrum time on purpose, so the
foam criterion (a fraction of g) keeps the model look at every scale.

## The geometry clipmap

Concentric square rings, **each with a FIXED world-space cell size that doubles per ring.**

> Because a ring's cell size is constant, every world position inside it samples the displacement maps
> at a **FIXED mip regardless of camera distance — wave shapes no longer morph with camera motion**,
> which was the failure mode of the previous radially-graded grid whose per-vertex mip followed
> distance.

**Ring transitions use a CDLOD-style vertex morph baked per vertex** (texcoord = ring cell size + morph
weight): over each ring's outer band, odd vertices collapse onto the next ring's coarser lattice and
the sampled mip blends +1, **so the boundary matches the next ring exactly — seamless by
construction.**

A FLAT "Horizon band" ring extends to the far plane so the sea always meets the horizon.

## Sectors

The clipmap splits terrain-chunk style: ring 0 whole, each outer ring as 8 rectangular blocks around
its hole, the horizon band as its 4 sides. **Each is ONE `RenderMesh` + node (no `ObjectContainer`;
every sector shares `m_material`, `createMeshMaterial(Ocean, rayTraced = false)`, no LOD chain — the
clipmap is its own LOD) with its own SpatialIndex entry** (`SpatialLayer_Terrain`, userData =
`&sector.node | SpatialTerrainTag_Ocean`, `spawnVisible = false`), so the CPU visibility gate and the
GPU per-instance frustum cull drop off-screen water **instead of vertex-shading the whole multi-km
disc every frame.**

* **A grid never moves in memory while a list may name it.** The entries register only after
  `rebuildGrid` has built the whole vector (no more reallocation). A replaced or released grid is
  RETIRED (`retireGrid`: entries, nodes, meshes released at once; the vector kept in `m_retiredGrids`
  until `visibleCollectGeneration` moves past its value, freed at the top of `update`).

* All sectors share ONE transform, **snapped to a lattice multiple so vertices re-land on the same
  world positions** as the camera moves.
* **Both culls pad their bounds by `displacementExtent()` every frame.** The mesh they were built from
  is the UNDISPLACED lattice; the vertex shader then moves each vertex by the wave height and by the
  CHOPPY horizontal displacement, which scales with "Choppiness". A sector's bounding sphere absorbs
  some of that incidentally — its XZ half-diagonal exceeds its half-width — and **that spare slack is
  what a high choppiness runs out of**, dropping sectors with their crests still on screen. It shows at
  the SCREEN EDGES, where the frustum planes cut closest to a sector's own sphere.
  * `estimateWaveExtents` (the sparse readback re-scan, every 16th frame, F16C-decoded 2 texels per
    `__m256`: ~0.1 ms scalar before) also returns the
    crest and the largest RAW horizontal displacement. **Raw on purpose**: the maps store Dx/Dz before
    the choppiness lambda, so the live tweak scales the padding with no re-scan.
  * The CPU side re-registers `baseRadius + extent` per frame; the GPU per-instance cull gets the same
    number through `u_oceanLive_displacementExtent` and adds it for `PIPELINE_IDX_OCEAN` instances
    (`instanced_indirect.cs.glsl`) — **frustum test only**, so LOD selection still sees real bounds.
* **Sector borders duplicate identical vertices, so splitting cannot open seams.**
* **Every triangle is emitted in BOTH windings** (`pushTri`), so the back-face-culled Ocean pipeline
  draws the surface from either side and the scene depth holds the nearest face from below too. (A
  cull-none variant shaded every wave crossing along an underwater ray — stacked "water planes".)
  Only the index count doubles. **So
  `gl_FrontFacing` is always true on the water**; the ocean shader takes the side from the triangle's
  plane instead.
* The Spatial Main gate skips off-screen sectors (ocean is `PASS_MAIN` only), and a "Dry sector cull"
  skips sectors fully buried AND inside the streamed-mesh radius. **The dry test is the node's pass
  mask**, set per sector every `update` on main (`PASS_MAIN`, or 0 = the push skips it), so whoever
  pushes the node needs no ocean logic.
* **`update` never walks the grid for the push, and mostly not at all.** The re-centering of the
  sector entries (transform + `updateEntry`, padded by `displacementExtent()`) runs only when the
  snapped position, the sea level or the pad changed — the snap steps by 8 cells and the pad
  refreshes every 15 frames — and stays on main (an `updateEntry` on a worker could race the pool
  growth by a later registration in the frame). `update` only prepares the render job's
  input (`m_cameraPos`, `m_renderReady`); **`render(culled)`**, called by `TerrainStreamer::render`,
  kicks the job. The main-visible sectors come from the Spatial hand-over, collect slot 1: the
  TERRAIN's render job walks it once and pushes the sector nodes with the chunks (from that worker;
  the sectors, masks and transforms hold until the next `update`, after the pre-present joins).
  Culling `Off` makes the ocean's own job push every sector instead. **A rebuilt grid draws from the
  next real stamp on** — one frame without water after a rebuild (rare: the ring tweaks, a far-plane
  change, re-enabling; frozen culling keeps it hidden until unfrozen), accepted over a second push
  path; the walk skips the old grid's destroyed nodes meanwhile.
* **The rest is a job** (`"oceanReadback"`, High, `m_renderCounter`; the culling-Off sector push has its
  own `"oceanRender"` scope inside; it reads `m_cameraPos` and
  captures only whether it pushes the sectors): the sector pushes only with culling Off, the displacement readback copy (`m_dispTile`;
  buoyancy samples it BEFORE the kick, and the readback slot is stable until present), the sparse
  wave-extent re-scan and the camera water-surface sample. `joinRender()` — main.cpp right before
  present; also `update`, `rebuildGrid` and the dtor — applies the three renderer stores (wave
  trough, displacement extent, camera water surface).
* **The horizon band is exempt from BOTH under-terrain culls** — a negated cell size flags its
  vertices, because its huge triangles break the vertex cull's footprint assumption.

## What runs where

Everything else is on the GPU: `OceanSimulationPipeline` (in RendererVK) simulates the spectrum and
IFFT into displacement and gradient maps **each frame, so every parameter is live**; the Ocean pipeline
variant displaces this mesh and shades it (RT refraction, Beer-Lambert). Params push through
`Renderer::setOceanParams`; **the mesh rebuilds only when ring params change.**

**The ocean bakes NOTHING itself** — the shore's surface weight, surf, swash and the land cull all read
the streamer's terrain-data map, and the GPU passes read the SAME bake the CPU copy comes from, **so
the drawn water and the simulated water agree by construction.**

**The shore is ONE depth weight on the raw cascade sum** (`oceanSurfaceWeight`, ocean_wave.inc.glsl):
1 in open water, easing to "Swash amplitude" (× sea-connection × land-height fades) across an approach
band sized by "Shoal depth scale". Every cascade is scaled alike, so the wave's spectral detail is
preserved into the beach; there is no per-cascade shoaling, no breaking limit and no waterline floor
any more — the surface is the wave, and the depth buffer cuts it against the sand. The swash weight
(the same base, faded in across the band) gates only the tongue's backflow.

## `sampleWaterHeight` — the buoyancy field

CPU-evaluated from the GPU displacement readback, and **a full CPU MIRROR of the clipmap vertex
shader**: the raw cascade sum times the shore's surface weight, plus the swash backflow. ~2 frames
latent — invisible for physics. The App wires it into `PhysicsWorld::setWaterSurface`.

> **The shader is what you see, the mirror is what floats on it. Changing either without the other is a
> silent bug** — bodies sink through drawn waves, or float on dry sand.

It returns `-FLT_MAX` where there is no water: ocean disabled, readback not primed, or land beyond the
swash run-up band. **Inside that band it returns the live tongue surface, which SINKS BELOW the terrain
as the wave recedes — so bodies beach themselves**, and callers want a plain surface-vs-point test
rather than a separate dry check.

`hasWater()` is the App's global gate for the buoyancy pass, **so a disabled ocean costs physics
nothing** — the pass otherwise sweeps the whole broadphase every step.

## Wind

The sea's wind is THE wind (RendererVK `WindParams`, "Sky/Wind"): its direction, and its speed x `Ocean/Waves/Wind
speed scale` (2.5) = the MODEL U10 (`windSpeed()`; the surf band, foam drift and calm cap all read it). The dominant
spectrum term travels AGAINST `windDirection`, so the sim takes the heading the wind blows FROM (`baseWindAngle()` =
"Sky/Wind" direction + π) and the swell heading is `swellTravelAngle()` = the wind's own direction (the default
113° keeps the former 5.12 rad sea). `steeredWindAngle` slews the SIM wind toward the baked shore flow so waves roll
inland. The app hands `swellTravelAngle()` to the terrain's flow bake every frame (`setFlowWindAngle`), so a wind
change reaches the next bake.

> Per-pixel domain rotation is **disabled** in `ocean_wave.inc.glsl` — it creases. Read the comment
> before reviving it.

## Near-field detail: three shading knobs

The FFT band ends at the finest cascade's Nyquist, and the mesh band-limits the displacement well
above that, so what is left near the camera is a normal map on a smooth surface — the "plastic" look.

* **"Ocean/Shading/Micro roughness"** — the slope variance of everything BELOW that Nyquist (the
  capillary band), added straight into the GGX `alpha²` as `2σ²`. **The LEAN term cannot cover it**:
  LEAN returns the variance the MIP CHAIN removed, which is exactly 0 at mip 0, so the near field fell
  back on the capped screen-derivative AA and then onto the 0.02 alpha clamp — a sky mirror, pixel for
  pixel what wind 0 looks like. Deliberately **not** scaled by "Glint filtering": that knob trades away
  filtered variance, and this band was never in the spectrum to filter. 0 restores the mirror.
* **"Ocean/Shading/Crest slope limit"** — `k` in the shading slope's soft limit `s /= 1 + k·|s|`
  (both `oceanSampleSurface` and `oceanSampleNormalLod`, which must agree). It keeps the near-fold
  division from exploding into dark creases, but it compresses exactly the steep crest faces, so
  **lower = sharper crests**, 0 = no limit. Only the SHADING slope: `oceanSampleDisplacement` has no
  such limit, so the CPU buoyancy mirror is untouched by this knob.
* **"Ocean/Shading/Detail strength / scale / fade / rotation"** — `oceanDetailSlope`: the FINEST
  cascade's own gradient field re-sampled at `scale ×` its patch size and added to the shading slope.
  Wave statistics at a shorter wavelength than the FFT band holds, for **one fetch — no extra memory,
  no extra FFT**.
  * The domain is **rotated**, because an unrotated copy is the same field scaled: its crest lines run
    parallel to the parent's at every point and read as a fractal repeat rather than as ripples. The
    slope is rotated back before `oceanFlowToWorld`.
  * It is **shading only** — never in `oceanSampleDisplacement` — so geometry and
    the CPU buoyancy mirror are untouched, and the shader/mirror rule is not broken.
  * Implicit LOD: a smaller patch makes the uv derivatives correspondingly larger, so the mip chain
    filters this band the way it filters the cascades. It still **fades out with distance**, because
    this band is absent from the LEAN moments — what the mips remove would simply vanish instead of
    becoming roughness. "Micro roughness" is what covers that band.
  * It is scaled by the shore surface weight, so it dies into the beach with every other wave term.

The first two are dimensionless (a variance and a slope ratio) and so is the detail strength, scale and
rotation — `pushOceanParams` world-scales only "Detail fade (m)".

* **"Ocean/Foam/Bubble depth (m) / Bubble brightness / Bubble blur (m)"** — the entrained-bubble cloud
  (`ocean_bubbles.inc.glsl`, 2026-09-30). Its coverage IS the foam field's amount (clamped; it also adds
  roughness, amount × 0.35). A "Turbidity" scale on it was removed as redundant with the brightness. The
  cloud is a high-albedo layer "Bubble depth" UNDER the surface, so the water over it absorbs the red both
  ways and it reads **turquoise, not grey** (it replaced a mix toward `whitewater × 0.55`). The ocean reads
  its coverage where the refracted view ray reaches that depth (one parallax tap); the terrain film uses the
  same function with the depth capped at its own water, so the hand-over keeps one colour. "Bubble depth"
  is a metre (× s); the brightness is a scale.
* **The surf band scales with the WIND** ("Ocean/Foam/Foam wind full (m/s)", `pushOceanParams`): its width
  "Shore foam depth" × smoothstep(0, full, MODEL U10) - off in a calm, full from "full" up. Its cap ("Shore
  foam max") scales only in a near-calm: × smoothstep(0.5, 2 m/s, model U10): none below 0.5 m/s, full from 2 m/s. The fold /
  breaking thresholds do not scale. (A wind shift of both thresholds - "Foam wind
  onset" / "Calm foam cut", also moving the surf band's waterline threshold - was tried and removed,
  2026-09-30.)
* **The WORLD-SPACE FOAM FIELD** (2026-09-30; `ocean_foam.cs.glsl`, `ocean_foam_field.inc.glsl`,
  `OceanSimulationPipeline::advanceFoamField`). It replaced the turbulence stashed in cascade 0's moments `.w`.
  That field was 3 m texels over the 1.5 km patch, and it showed only by relaxing the live fold threshold,
  so aged foam stayed on the crests.
  * `OCEAN_FOAM_LEVELS` (3) camera-centred clipmap levels of 512², texel "Foam texel (m)" × 4^level (0.5 /
    2 / 8 m: 256 m / 1 km / 4 km). They are the ocean MAPS' last layers (`3 × CASCADES + level`, mipped
    by the blit), so any maps sampler reads them - no new binding. Ping/pong state: an R16F image, layer
    = level × 2 + slot.
  * ONE channel: the FOAM AMOUNT (`max(prev × "Foam decay", instant)`, no spread), injected by
    `oceanInstantFoam`. Above "Foam threshold" (its density, below) it draws white foam; the same amount is
    the bubble cloud and the roughness. So decaying foam fades into the turquoise glow.
    (A second turbulence channel with its own decay / spread, and "Foam boost" - turbulence relaxing the
    live fold threshold, the old aged foam - were removed as redundant, 2026-09-30.)
  * The bubble cloud reads the amount BLURRED ("Bubble blur (m)", default 4, × s): a diffuse volume, so
    the separate breaking events merge into clouds instead of texel-sized spots. That read picks a coarse
    mip and reconstructs it with two mips' B-splines (8 taps); a magnified coarse mip read bilinearly
    shows its texels as diamonds.
  * Indexed in DRIFTED REST coordinates, q = rest XZ − drift. The rest (undisplaced) lattice is where a
    parcel sits between orbits, so foam on it rides the waves and STAYS as a crest passes on. The drift
    ("Foam drift (% wind)", along the swell's travel, over the SIM delta) moves the whole frame, never
    resamples, so it never blurs.
  * The CPU state scrolls each level by whole texels (the UBO's `zw` = the compute's read offset). A texel
    change or a frame with the ocean off resets the field. A texel that SCROLLS IN starts from the next
    coarser level's last-frame state (bilinear), not empty: at a long "Foam decay" an empty strip took
    seconds to fill and showed each level's edge as a line trailing the moving camera. The outermost level
    starts empty (it fades out anyway).
  * The ocean draws `max(crest foam, surface foam × "Surface foam")`; the film does the same inside its
    shore gate. The film samples at its own position (near the shore it is within a displacement of the
    rest lattice).
  * **Coverage is DATA-DRIVEN** (`oceanStuckFoamDensity` / `oceanStuckFoamCoverage`): a threshold
    ("Foam threshold", "Foam edge") on the stuck foam's surface density, `amount / J`. The live fold
    Jacobian J is the displaced area per rest area, so the foam packs where the water converges and
    tears where it stretches. Every cascade is in J, so the edges get detail down to centimetres with no
    pattern. The ocean widens the edge by `fwidth(density)` (AA); the film has no derivatives there and
    relies on its mip-filtered Jacobian taps. The J it reads is `foamJacobian` (`oceanSampleSurface`): the
    finest cascade scaled by "Foam fine waves" (0.25). Its sub-second waves reshaped the foam every frame
    ("too active"). The film's crest fold and shoreline lace read the same damped sums. A procedural lace (ridged noise) was tried first and
    REJECTED by the user: they want data-driven foam, not a pattern.
    The field is read through a cubic B-spline where magnified (`oceanFoamBicubic`).
  * **The foam's lighting normal** (ocean): its own slope, `macro × (1 − "Foam flatten") + detail ×
    "Foam detail"`. `oceanSampleSurface` returns the sub-band detail slope separately for this. The flatten
    eases only the large waves, whose bent crest facets otherwise go dark, and the detail keeps the foam's
    relief below the geometry. A bump from the lace's gradient was REJECTED (it looked bad).
    **The film's whitewater is the ocean's exact formula** on the same slope rule (`TerrainFilm::foamSlope`,
    at the OCEAN's normal strength, not the film's normal scale or waviness) with `sunSurfaceRadiance()`
    as the sun, so the two foams meet in one tone.
  * **The surf band's cap** ("Ocean/Shore/Shore foam max") is a soft knee NORMALISED so full lace reaches
    the cap (ocean + film). The bare knee `fm (1 − e^(−x/fm))` stopped at 1 − 1/e ≈ 0.63 even at cap 1, so
    the whole "Shore foam depth" band was a grey 60 % veil beside the fully covering stuck foam — read at
    first as a shading difference (2026-09-30). The band's TARGET (nearShore × bore, the waterline term)
    fades the lace in through its fold THRESHOLD only, `b = mix(Fold bias − 0.8, 1.45, target) + Shore
    foam bias`, never as a coverage multiplier: multiplied, the band's fade was a half-transparent veil. At
    target → 0 only folds 0.8 past "Fold bias" foam, a subset of the crest foam, so the hand-over has no
    seam. The film keeps `shore` (the land gate) as a multiplier: inland its raw Jacobian is still the full
    open-sea fold field.
  * Not shore-weighted: injection is open-ocean math, so the stuck foam and the milk can show in the calm
    shallows where the waves were damped.
* **`OceanParams::cameraUnderwater`** (`u_oceanLive_cameraUnderwater`) — set each frame in `pushOceanParams` from
  `sampleWaterHeight` at the camera. `ocean.fs.glsl` takes its UNDERSIDE path only while it is set: a back
  face seen from above is a FOLD (high "Choppiness" overturns the sheet), and before this gate it shaded as
  the underside, with half-bright "foam from below" (dark grey sheets on the curls). It now shades as the top side.

"Ocean/RT" tweaks budget per-pixel scene rays.

---

# `TerrainCollider`

"Terrain/Collision" tweaks. A **focus-centered** ring of small static triangle-mesh collider tiles.

**Sampled from the SAME `ITerrainSampler` the terrain renders**, on the render LOD0 lattice with the
render mesh's triangulation, **so bodies rest exactly on the drawn surface.**

It **never touches the render chunks**: a LOD0 chunk is ~500k triangles, and a collider only needs the
ground near dynamic bodies. Tiles are a few tens of metres and exist only within "Radius" of the focus
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
step) must not overlap a body create/destroy. Between the kick and the join only ocean / trees / rocks / clutter /
particles / force / UI kick run on main — none touches box3d. Sub-scopes `"Terrain collider clear"` /
`"... create body"` / `"... evict"` show the box3d cost.

Tiles clear and rebuild on a sampler identity change, exactly like the streamer's residents; eviction
has half-tile hysteresis. `maps == nullptr` clears everything (on main, once, after joining the update
job). The dtor waits out the update job, then the in-flight build (main helps).

Verify with `Physics/Debug/Draw colliders`.

---

# Trees

"Trees" tweaks. **In development — the plan is `Docs/TreeRenderingPlan.md`** (tiers T0..T3, phases G1..G8).
Status: G1 (species asset + piece generator) and a CPU composite PREVIEW.

## The model: a piece library, composited per tree

A tree is NEVER generated as a whole. A species generates once into a `TreeLibrary`
(`generateTreeLibrary`, TreeGenerator.cpp): **trunks** with attach **slots**, and **branch modules** (a main
branch with its sub-branch levels and leaves). `compositeTree(species, library, seed)` then picks a trunk and
fills its slots with modules: module +Y onto the slot direction, module +Z (its "up side") toward world up,
a roll jitter, a scale to the slot length. **The composite's only randomness is `treeHash` (PCG)** — the GPU
version (phase G4) must reproduce it bit for bit; keep the two in step.

* Pieces carry a per-vertex **bone** index (piece-local; bone 0 = the piece root, each level-1 sub-branch of a
  module gets its own) and `TreeBone` pivots/axes — the data the wind and the GPU bone palette will use.
* Modules are generated in a frame where world-up is `(0, cos φ, sin φ)` for the species' mean slot angle φ,
  so up-attraction and leaf facing are world-relative after the composite.
* Slot lengths follow the crown envelope (`Crown Shape`) — the envelope sets the silhouette, modules fill it.

## `.tree` — species asset (`Assets/Trees/*.tree`, `loadTreeSpecies`)

```
TreeSpecies <name>
	Seed n · Scale min max · Kind Tree|Bush (default Tree; a Bush is scattered around the grove's trees, never one)
	Climate <name> (a Bush grows around the trees of the same climate only, case-insensitive; a tree whose
	        climate no bush shares takes every bush)
	Trunk   Count · Height min max · Radius · Taper (tip/base) · Flare · Sink (m below the base, default 0.5) · <shape>
	        Lobes n (ridges/buttresses around the trunk; 0 = round) · LobeDepth base top (radius fractions)
	        LobeHeight (height fraction over which the ridges fade to the top depth) · Twist (deg over the height)
	Crown   Shape Ellipsoid|Cone|Umbrella|Column · Start (fraction of height) · Radius (m reach)
	        Slots · Fill (probability) · Angle bottom top (deg from up) · AngleVar · Leader true|false
	        BranchRadius (max module base radius / trunk radius at the slot; caps the module scale)
	        Tiers count spread (slots grouped into `count` whorls up the crown, each spread over `spread` of
	        its band; envelope and angle follow the whorl's height - layered crowns; default 1 0.35)
	Module  Count · Length (m, 0 = crown radius) · Radius (base / length) · <shape>
	        Level { Count · Start · Length · LengthTaper · LengthVar (each child a random 0..this shorter,
	        0..0.95, default 0) · Angle value var · Rise (0..1: the start direction blended toward world-up -
	        strands that arch over before they hang; default 0) · Radius · <shape> }  (up to 3)
	Leaves  Size (m) · Aspect (width/length) · PerBranch (per average last-level branch; other branches
	        scale it by their length) · Levels (deepest tiers that carry leaves; 1 = last level only)
	        Type Single|Cluster · Cross true|false · Style Leaves|Needles|Pinnate · ClusterLeaves (Needles: side
	        shoots per branch; Pinnate: pinna pairs per compound leaf) · Shoots (Needles / Pinnate: branches /
	        compound leaves fanned from the card base, 1..5, default 1) · ClusterLeafSize (Needles: the needle
	        length; Pinnate: the leaflet length; 0.02..0.6) · NormalBend (0..1, default
	        0.7: card normals bent away from the module root, both faces)   (Cluster only, below)
	        Align (0..1, default 0: cards stick out of the branch facing up; 1 = they lie ALONG the branch,
	        facing out of it at random turns - a weeping strand's curtain)
	Lod     ErrorScale (x the per-level error; > 1 = coarser levels nearer the camera)
	Bake    Variants (baked whole trees per species, default 4)
	Billboard Distance (m, 0 = none; default 50) · Resolution (px, power of two, 512) · NormalBend (0..1, 0 - the user's pick, every species)
	          FadeWidth (m, the mesh <-> billboard crossfade band centred on Distance; 10)
          AlbedoScale (x the WHOLE-TREE billboards' colour, linear, 0..8; default 1 - applied after the cache, at
          load: a change needs no re-bake; the module billboards / branch cards and the far volume keep theirs)
          TopCardHeight (the WHOLE-TREE billboard's horizontal card, a fraction of the tree's height: 0 = bottom,
          1 = top; default 0.5 - its bake is orthographic from above, so only the card mesh moves: no re-bake)
	Bark    Plates lines rows (fissure lines per family around / horizontal-break rows along, per tile)
	        Crack (fissure width, ridge fraction) · Breakup (0 continuous lines .. 1 short segments) · Relief · Lichen
	Color   Bark r g b · Leaf r g b
	Placement (WORLD placement, TreeWorld; trees only - absent or Density 0 = never placed in the world)
	        Density (per ha in the ideal climate) · Temperature min max (C; the IDEAL range, one value = exactly that)
	        Precipitation min max (mm/yr; the ideal range - 2200 and up is the encoding's top, so a max there is no max)
	        ClimateWidth (sigma of the falloff OUTSIDE the ideal box, in the normalized climate space, 0.06)
	        Cluster size coverage (m, 0..1; size 0 = no patches) · MaxSlope (rise/run, 0.6)
	        Altitude min max (m above the local water level, 1.5 ..)

<shape> (TreeBranchShape, any of): Curve value var (deg, smooth bend) · UpAttract (negative droops - weakly:
        -1 over the length bends a horizontal branch only ~45° down)
        Droop (deg turned toward straight DOWN over the length, a fixed rate that stops at vertical: 400 = the
        strand hangs after a quarter of its length; 0..720, default 0). A species with ANY Droop grows each
        module for its own pitch (spread over the slot angles ± AngleVar, not the mean), and the composite
        places the nearest-pitch module AT that pitch with no roll jitter: the module's down is then exactly
        the world's (with the mean pitch and ±25° roll the strands hung up to ~50° off plumb).
        Wobble (deg random walk per segment) · Elbows (average count) · ElbowAngle value var (deg)
        ElbowUpBias (probability a downward elbow turn is mirrored upward; default 0.5)
        ElbowMinElevation (deg vs horizontal an elbow may turn down to; default -15; a branch that already
        points lower before the elbow is not lifted) · ElbowRange start end (length fractions; default 0 1)
        Stubs (probability an elbow carries a snapped-off stub) · Rings · Sides
```

**Elbows** are sharp turns on interior ring nodes, each rounded by replacing the corner with three points of
a quadratic Bezier (+2 rings per elbow); everything along a branch is addressed by its arc-length fraction
`u`, not by ring index. A **stub** continues the direction the branch had BEFORE the elbow (the branch that
snapped off), short and thick, with a flat broken end cap.

Placeholders: `Oak` (ellipsoid, cluster cards), `Pine` (cone + leader from 12 % of the height, 60 slots at 85 %, two
sub-branch levels (7 + 4) drooping, 1.1 × 0.83 m NEEDLE-spray cards of 3 fanned branches, 2 per branch, on all
three tiers (the module roots too: near the top of the cone the short modules are mostly root) -
reworked 2026-10-03 from single diamond leaves on one level, which read as bare poles), `Acacia` (umbrella:
14 slots, sub-branch levels 10 + 5 up-attracted, flat 0.9 m PINNATE cards, 4 per branch - 1 compound leaf of 4
pinna pairs, leaflets 0.07 of the card (~6 cm; true-to-life ~2 cm leaflets were invisible) - on the last two tiers; reworked
2026-10-03 from 0.1 m single diamonds), `Willow` (weeping: an 8.5-10.5 m trunk carrying 20 thin limbs in TWO whorls - `Tiers 2 0.4`,
Ellipsoid envelope (upper reach ~84 % of the lower; Cone gave ~33 %), Angle 55 30, trunk Taper 0.35 + BranchRadius
0.7: the thin upper trunk caps the upper modules to ~80 % - the upper layer's size knob -, arching over - Droop 80 -, 16 arcing branches each - Rise 0.3, Droop 120, leaved
too -, 7 strands per branch that rise, arch over the dome and hang plumb - Length 2.6, Rise 0.7, Droop 280 - of random length
(`LengthVar 0.65`: curtains ending at layered heights), narrow cluster cards along them, `Leaves Align 0.8`;
~1000+ cards per module, ~5x Oak).
Climates: `Oak` Temperate, `Pine` Boreal, `Acacia` Savanna, `Willow` Temperate. Bushes (`Kind Bush`), two per climate:
* Temperate: `Shrub` (broadleaf, round, sparse cluster cards) and `Thicket` (denser and wide: crown radius 1.5 m,
  flat slot angles).
* Boreal: `Juniper` (a low SPREADING conifer, blue-green needle-spray cards, uncrossed - crossed read too dense; the first conifer bush, on
  single diamond leaves, was removed - the needle texture made it work) and `Bilberry` (knee-high, many thin upright
  stems with small dense leaf clusters).
* Savanna: `ThornScrub` (low, wide, umbrella crown, zig-zag grey stems, pinnate pads) and `Bushwillow` (an upright
  vase of stems, dry olive-yellow broadleaf clusters, up to ~2.5 m).

A bush is a
tree species with a short trunk (`Height` ~1 m), slots from the ground (`Crown Start 0`), a large `BranchRadius`
(the thin trunk would otherwise cap the module scale) and a short `Billboard Distance`.

## Piece mesh LODs

Every piece has `TREE_PIECE_LODS` (4) mesh levels, all from ONE skeleton: generation first records a
`PiecePlan` (branch paths, stubs, leaf placements — every random decision, in the old RNG order), then
`meshPiece` meshes each level from it (`PIECE_LODS` table in TreeGenerator.cpp):

| Level | Rings / sides | Branch levels | Leaves (every Nth, scaled √N: same area) | Stubs | Error (× length × ErrorScale) |
|---|---|---|---|---|---|
| 0 | 100 % / 100 % | all | 1 | yes | 0 |
| 1 | 60 % / 60 % | all | 2 | yes | 0.001 |
| 2 | 40 % / 45 % | deepest dropped (never below level 1) | 4 | no | 0.0025 |
| 3 | 25 % / 34 % | two deepest dropped (never below level 1) | 8 | no | 0.005 |

**The GPU uses LEVEL 0 ONLY (2026-10-03): no `createMeshLodChain` for tree pieces** (`uploadLodChain` uploads level
0 and leaves the chain empty). The trees switch through their own tiers (mid tier, billboard, far volume); the mesh
LOD steps underneath popped visibly (fewer, larger leaf cards per level, no crossfade). The generator meshes level 0
only (`MESHED_LODS` = 1 in TreeGenerator.cpp; the table below stays for a return of mesh LODs); `lodError` / `Lod
ErrorScale` are unused, and RendererVK allocates no hysteresis slots for tree records.
Debug: `Trees/Debug view` (RendererVK) colours the lit meshes per material / mesh / fade side.

## Far representations (`Trees/Far mode`)

The SHARED pieces — modules AND trunks, and the baked whole-tree variants — get a billboard each (a trunk's +Y is
world up, so its billboard's "top" card stands vertical: two crossed vertical trunk cards; cache names
`<species>_trunkbillboard…`, modules keep the unprefixed ones), crossfaded in per placed piece beyond its distance ×
`Trees/Far distance scale` (`Trees/Force far` shows them all). Two modes, `Billboards` and `None`; changing the mode
reloads. (The octahedral-impostor fallback mode was removed 2026-10-04.) The bakes live in `TreeImpostor.cpp` (the
file name is historical): a small software rasterizer (2×2 supersampled, back faces culled, leaves alpha-tested);
`treeBakeHash` (geometry + `BAKE_VERSION` + settings) names the cache files.

* **Billboards** (default): two crossed cards along the module axis from its LOD-0 box — a vertical card
  (normal +X) and a horizontal card (normal +Z = world-up after the composite), their views stacked as strips
  of one square texture (`bakeBillboards`, `billboardMesh` in TreeImpostor.cpp). `Trees/Billboard views`
  (reloads): **2** (default) — a strip per card (side, top), the back face showing the front view through the
  card; **4** — each FACE its own strip, top to bottom: side from +X, side from −X, top, bottom. A back view
  keeps the card's right / up and negates only the view direction: stored as seen THROUGH the card, it reads
  the right way round on the back face, and its tangent-space normals match the back face's TBN (same tangent /
  bitangent, the handedness flips) — no shader change. Mips stop at 4 px per strip. Cache names carry the view
  count (`<species>_billboard<i>v<views>_<hash>`), so both settings keep their files. Both card PLANES pass through the branch axis (module x = 0 / z = 0),
  not the box centre, so the root lands at the trunk. **Whole trees** (the baked variants, whose +Y is up, so
  both cards above stand vertical) get a THIRD, horizontal card for the top-down view
  (`billboardHorizontalView`): across the axis at mid height, seen from above, u (+Z) spanning at least the
  tree's height so the lit FS's crown radius matches the vertical cards'. The material carries
  `MATERIAL_FLAG_BILLBOARD_TOP_CARD`; the lit FS finds the card by its up-facing normal (RendererVK). The
  edge-on fade hands over between the vertical cards and it. **Strip layout** (`billboardLayout`): 3 cards
  share the texture (3 strips with 2 views, 6 with 4), each strip's height rounded DOWN to a multiple of the
  largest power of two p that leaves ≥ 4 px per strip at mip log2(p) — so every strip boundary lands on a texel
  boundary at every mip the material gets and no mip mixes two cards. 512 / 3: p = 32, 160 rows a strip
  (+25% over a 4-strip layout), 32 spare rows at the bottom. 2 cards: 256 rows, mips to 4 px as before. **THE CROWN FIELD** (`buildCrownField`, TreeImpostor.cpp,
  2026-10-04): the piece's LOD-0 leaves (its bark when it has none) splatted by area into a grid of cubic cells (32
  along the longest side), Gaussian-blurred (sigma 2.5 cells, ~8 % of the longest side) and normalized to a peak of
  1 - a soft hull of whatever shape the leaves make (round oak, cone pine, flat acacia pad). Per texel, at its leaf's
  3D point (the card point + the baked depth): the **hull normal** = the field's outward gradient (the box centre
  where the field is flat), and the **interior** = the optical depth out along that normal / the field's CORE depth
  (from its peak out to the side, the mean of 4 horizontal directions), clamped to 0..1 - linear, so the lit FS's
  `Foliage interior depth start / end` remap it, and per-tree normalized, so a narrow pine and a wide oak both reach
  ~1 at their core. **VOLUME normals**: each texel's normal is bent by `Billboard NormalBend` toward the hull normal
  (was: "out of the box centre" - on a tall tree that baked normals pointing up at the top and down at the bottom) and
  keeps its sign, in each card face's tangent space. **The normal image is the CROWN layout**: RGB = that normal,
  **A = the interior** (RendererVK: remapped, x `Trees/Foliage interior shadow`); the mips are the bark's
  `buildNormalMips` (RGB renormalized, A averaged). The material carries
  `MATERIAL_FLAG_BILLBOARD`, so the lit FS does not reject the sun shadow by the flat card normal — without
  both, a whole card went dark whenever the sun was behind it. (The normal map's ALPHA was the texel's depth off the
  card until 2026-10-04 - read by no shader since the shadow pass's pixel depth offset was removed on 2026-10-02; the
  bake still computes it per texel, for the leaf's 3D point in the crown field.) The billboard materials and the leaf cluster material also carry
  `MATERIAL_FLAG_LEAF`: the sun shines through them (RendererVK leaf transmission, `Trees/Foliage
  transmission*` tweaks). The lit FS also blends their normals toward a view-ray CROWN normal (an ellipsoid of the
  card's proportions; `Trees/Foliage crown normal`), so the two cards stop shading differently at their crossing axis. This
  needs the card's +u axis to run from the instance origin (`billboardViews`: right = piece +Y). Real geometry on
  `LitFoliage` (RendererVK: LitMasked + the card paths; a billboard material must use it): **they cast
  their own alpha-tested shadows**, so the mesh nodes stop drawing entirely.
* **Crossfade** (billboards only): a band `FadeWidth` wide, centred on the billboard distance × `Far distance
  scale`. Inside it (centre distance ± the module radius) the module draws its mesh as `barkFade` / `leavesFade`
  — the same LOD-chain meshes on `LitMasked` with species materials DERIVED with a distance fade-OUT
  (`Renderer::deriveMaterial`, shared textures) — plus the billboard, whose material fades IN over the same
  band. The lit FS's dither (RendererVK `MATERIAL_FLAG_DISTANCE_FADE`) gives each pixel to exactly one of them;
  TAA blends the per-frame dither. Outside the band only one side draws, the mesh on its normal (early-depth)
  materials. `applyFadeBands` writes the bands into the material flags (at reload and when the distance scale
  changes — no reload). Not dithered in the shadow pass: inside the band both cast. `Force far` keeps the
  hard switch. (Turning the cards about the
  branch axis toward the camera was tried and removed — the user did not like the look.) Cache `<species>_billboard<i>_<hash>.png` (+ `_normal`), mips down to 8 px.
* **RT (GI, RT shadows, RTAO, reflections) sees only the WHOLE-TREE billboards** of TREE species: every other tree
  mesh is created without a BLAS (`createMesh(..., raytraced = false)`) - bark / leaves / trunk / branches, the card
  meshes, the module / trunk billboards (the library rows) - and BUSHES never get one. Without
  billboards (Far mode None) the tree species' bark + leaves are raytraced instead (they stand in for the
  tree in shadow + GI there).
* **Branch cards — the MID tier** (GPU path, baked variants; `Trees/Branch card distance`, default 0.4 × each
  species' billboard distance, 0 = off; respawns): every baked variant keeps its composite (`TreePiece::placements`)
  and its bark SPLIT in two (`trunkBark` - the trunk alone - and `branchBark` - the modules'; `bark` stays whole for
  the bakes and the CPU path). `buildBillboards` stacks the module billboards into ONE CARD ATLAS per species (module
  m in rows [m, m + 1) x size; each mip the stack of the modules' own coverage-preserved levels) and merges per
  variant ONE CARD MESH: every module placement's cards in the variant's space, v into the module's block. Between
  the full mesh and the whole-tree billboard the tree draws the TRUNK (shared by both - it never fades at the mid
  band) plus the CARD mesh: over the mid band (`FadeWidth` wide) the branch bark (`branchMidFadeMaterial`) and the
  leaves (`leafMidFadeMaterial`) fade out while the cards fade in (`cardInMaterial`); over the far band the trunk
  (`barkFadeMaterial`) and the cards (`cardOutMaterial`) fade out while the whole billboard fades in (derived
  materials, bands in `applyFadeBands`). The cards draw on LitFoliage with the whole-tree billboards' shading and
  "Foliage ..." tweaks, but with `MATERIAL_FLAG_NO_EDGE_FADE`: no edge-on fade (the user's call). Their crown frame
  is still per MODULE: `billboardMesh(..., axisInZ)` puts each card's axis (its module's +Y line, as a texture v) into
  texCoords.z = 3 + v, which rides the tangent's w magnitude to the lit FS. MAIN pass only: shadows, GI and RT keep
  the whole billboard. 4 fixed record slots per tree, no pool (RendererVK "BAKED TREE RECORDS"). The CPU path (GPU
  expansion off) has no mid tier.
* **None**: mesh LODs only.

## Texture files (`Assets/Local/Trees/Textures`)

Generated output under `Assets/Local`, so NOT in git: a fresh checkout generates them on the first load.
The species textures are generated ONCE and saved as PNG (`File:ImageIO`): `<species>_bark.png`,
`<species>_bark_normal.png`, `<species>_leaves.png` (cluster species only). Later loads read them back and
only rebuild the mip chains (bark box-filtered, leaves coverage-preserving). **A file that exists wins —
changing `Bark` / `Color` / cluster parameters does NOT regenerate it**: press `Trees/Regenerate textures`
(overwrites all) or delete the file. The files are also where authored replacements go: square power of
two, RGBA8; the leaf atlas keeps the 2×2 cell layout (stem at the TOP edge of each cell, v = 0); the normal
map is tangent space with x along u (around) and y along v (along the branch, DOWN the image).

**GPU formats** (`Trees/Compress textures`, default on, reloads): the PNGs stay the RGBA8 cache; at load the
finished mip chains (coverage-preserving alpha, AlbedoScale applied) are BC-compressed in memory (File
`TextureConvert::compressBlockRows`, stb_dxt, every level's block rows a `parallelFor` task, "Tree texture BC"):

| Texture | Format |
|---|---|
| bark albedo | BC1 sRGB (opaque) |
| bark normal | BC5 (XY; MATERIAL_FLAG_BC5_NORMAL rebuilds Z) |
| leaf cluster atlas | BC3 sRGB (alpha-tested) |
| billboard + card-atlas albedo | BC3 sRGB |
| billboard + card-atlas normal | BC3 linear - RGB the full normal WITH its sign, A the baked interior: no two-channel format holds both, and there is no BC7 encoder |

4x less VRAM (BC1: 8x). Off = the RGBA8 chains as before (the A/B for compression artefacts). The module / trunk
billboards are uploaded only with `Show piece library` (they draw only there; that tweak reloads too).

## Bark texture

PROCEDURAL per species until authored textures exist (`TreeBarkTexture.cpp`, 1024²): tiles in both directions,
matching the bark UVs (u once around a branch, v along it per base circumference — so the pattern scales
with each branch). **Furrows, not cells** (a Worley plate pattern read as scales): two families of fissure
lines along the branch, each meandering by its own tileable fbm, cross and merge into braided ridges. All
noise is tileable GRADIENT noise — smoothstep value noise has zero slope on every lattice line, which the
normal map showed as evenly spaced horizontal bands. Every
line fades out where its own per-column noise is high (`Breakup` → segments with gaps) and varies in width
along its length; a third, 3× denser family of FINE cracks (mostly gaps, shallow) splits the ridges; a
fourth, 8× denser family of STRIATIONS (thin, very shallow, short-to-medium segments) runs along the ridges; rare
thin horizontal breaks (20 % per ridge column and row). **Low contrast in the cracks** (albedo 75 % of
the bark colour, fine cracks 85 %; fissure floor at 35 % of the ridge height): dark or deep outlines read as
cartoon ink; rounded ridge crowns with V fissures, ridge-scale
height/tint from SMOOTH noise (a per-segment hash keyed by row/column made a brightness grid), fine grain, along-the-branch fibres and lichen on the ridges. sRGB albedo plus a
LINEAR RGB tangent-space normal map from the same height field (x along u, y along v — the tube's tangent /
bitangent), each with a CPU box-filtered mip chain (normals renormalized). Uploaded through
`Renderer::createTextureMaterial(..., &normalMips)`, freed with the species.

## Leaf types

* **`Single`** — one double-sided diamond per leaf, the species' solid leaf colour, `LitOpaque`.
* **`Cluster`** — alpha-tested CARDS (`LitMasked`), each showing a twig with `ClusterLeaves` leaves. `Size` /
  `Aspect` are then the card's, `PerBranch` counts cards, and a card's stem starts at the branch centre (the
  texture's twig grows out of the wood). `Cross true` adds a second card at 90° about the stem axis.
  The texture is PROCEDURAL until authored textures exist (`TreeLeafTexture.cpp`): per species a 1024² 2×2
  atlas of cluster variants (stem at v = 0, each card picks a cell), sRGB colours from `Color Leaf` / `Bark`,
  and its OWN mip chain — every level's alpha is rescaled (binary search) so the fraction of texels passing
  `TREE_LEAF_ALPHA_CUTOFF` matches level 0, otherwise the crown thins with distance. Uploaded through
  `Renderer::createTextureMaterial`, freed with the species (`TreeSystem::clearAll`).
  **`Style Needles`** (conifers, `drawNeedleCluster`): each cell is a conifer SPRAY instead of a leafy twig - `Shoots`
  needle branches fanned from the stem (the middle one along v, the others over up to ±18° and a little shorter;
  one card then reads as a whole spray, so the tree needs fewer branches and cards), each a main shoot plus
  `ClusterLeaves` side shoots (alternating, short, 24-38° off it - wider read as a flat fan), all densely set with
  thin tapered needles on both sides (two-segment, curving toward the shoot tip; shorter and steeper toward it; the
  far side darker) and a forward tuft at the tip. `ClusterLeafSize` is the needle length. Every leaf / needle /
  shoot is shortened until it fits inside its cell. **`Style Pinnate`** (acacia, `drawPinnateCluster`): `Shoots`
  BIPINNATE compound leaves fanned from a short woody twig (±28°), each a thin greenish rachis with `ClusterLeaves`
  pairs of LONG pinnae (0.55 × the rachis, forward, shorter toward the tip; the first pair close to the
  twig - a long bare stem read as a stalk). **Each of the 4 cells has its own CHARACTER** (`PinnateVariant`, the
  four read as copies with jitter only): a lean (±12°), a rachis curve (±0.16 × its length) and length (0.78-1),
  ±1 pinna pair, a pinna angle (30-50° + 0-18°) and length (× 0.8-1.12), the leaflet size (× 0.85-1.15), half of
  them ALTERNATE pinnae (the right-hand ones staggered outward) and a tint (× 0.88-1.12), every pinna a dense COMB of
  narrow oblong leaflets (`ClusterLeafSize` long, 0.4 as wide, 72-84° off the pinna, side by side with no gap,
  shorter in the pinna's last quarter; one half a little darker) - fern-like fronds, after a photo of a real
  acacia branch. (The first version - short pinnae nearly at right angles with sparse wide leaflets - read as
  scattered specks.) Every pinna fits its cell too. (The cell clips the drawing - the Oak's tip leaf lost its
  tip, 2026-10-03; the leafy twig also ends low enough for its tip leaf.)

## Baked tree variants (what the grove places)

The runtime no longer composites pieces per tree: at load every species bakes `Bake Variants` (4) whole trees
(`bakeTreeVariant`: the composite of a seed, all its pieces' bark merged into one mesh and all their leaves
into another, PER LOD LEVEL, at scale 1; each level's error = the largest piece error × its placement scale).
A variant is just one more "piece" (`Species::variants` / `variantMeshes`): LOD chains, a billboard (cache
infix `tree`), the crossfade — all the piece machinery applies unchanged. The grove places one
variant per tree with a seeded variant, scale (`Scale`) and yaw: **one GPU expansion piece = 4 records per
TREE** (was ~16 pieces × 3). A whole-tree billboard is two crossed VERTICAL cards through the trunk axis (a
tree's +Z card stands vertical too) — thin from straight above. Near-range uniqueness is planned as a per-tree
vertex-shader warp (bend / twist / lopsided crown / noise, seeded by position) that also carries the wind.
The pieces still exist (the bake input, the piece library rows).

**The WIND payload** (RendererVK "Tree wind", `tree_wind.inc.glsl` for the bit layout): `meshPiece` writes every bark
and leaf vertex's `texCoords.z = 1 + payload / 2^22` (`windPayload`): the module weight along the module's root bone
(a sub-branch's vertices take it at their bone's pivot), the level-1 sub-branch weight along its bone, the bone's phase,
a leaf card's TIP flag (corners 2 / 3; a diamond's all but the stem) - a card's weights from its STEM, so all four
corners share one phase. Trunk pieces: the wind bit only (the trunk bend). `bakeTreeVariant` ORs each module
placement's phase (3 bits) into its vertices. No vertex grows: the magnitude of the tangent's w was free.

## GPU expansion (G4)

With `Trees/GPU expansion` (default on), `spawnPreview` builds
ONE RendererVK tree instance set: a piece TYPE per library piece (its bark, leaves, the derived fade-out
materials and the billboard, plus the band), then every placed piece (transform, far centre + radius, type).
The set is uploaded once to device-local memory, and the culls make the per-piece decision and build the records
themselves, and the TLAS writer their RT instances (see RendererVK "BAKED TREE RECORDS"). **The pieces live in
the TERRAIN CHUNKS:** `spawnPreview` sorts them by the terrain chunk under their base (`Globals::terrain.chunkSize()`;
a chunk-size change respawns), one set chunk per terrain chunk, and hooks the set into the terrain
(`TerrainStreamer::setVegetation`, above): the terrain's render walk lists the chunks it draws with their passes,
and its sink calls `renderTreeInstanceSet` with that list - so the culls only see the plants of the visible
chunks (all passes) and of the shadow / GI sphere's chunks (shadow + GI). The sink runs on the walk's worker: it
reads the band settings through atomics. `update` binds the set (`bindTreeInstanceSet`, main) and submits
EVERY chunk itself when the walk does not carry it (terrain off, or the respawn frame - the new hooks start
with the next walk). Unhooking (`destroyTreeSet`) joins the walk. Every baked variant also carries its
FAR-TREE VOLUME grid (`bakeTreeDensity`, `TREE_DENSITY_RES` = 32³ extinction over its billboard box, from the
LOD-0 LEAF triangles only; cluster leaves count half opaque. The bark was dropped 2026-10-02: a trunk's whole area
in one thin voxel column drew dashed vertical streaks through the far blobs) and the species leaf colour — the leaf texture's
alpha-weighted mean, DECODED FROM sRGB (`meanLeafAlbedo`; the albedo textures upload as sRGB, so the raw
`leafColor` taken as linear read light yellow-green) — handed to the set as
`TreeInstanceType::density` / `albedo` (RendererVK "Far-tree volume"). With billboards, the BILLBOARD is the
piece's only shadow / GI / RT representation at every distance: the mesh draws in the MAIN pass only (both
paths). Off, it keeps the CPU path below (one
`RenderNode` per piece representation, pushed per frame). The set is destroyed on respawn / reload / disable.

## World records (`TreeWorld`, "Trees/World" tweaks; W1 of Docs/TreeRenderingPlan.md 3.5)

Every tree of the terrain ring as a **4-byte record per terrain chunk** (`TreeRecord`: x, z 12 bits each, chunk-local
on a 4096 lattice; the species type 8 bits = its index in the NAME-SORTED `.tree` list). No height (the terrain's), no
variant / scale / yaw: `treeRecordSeed(seed, chunk, record)` hashes the chunk and the QUANTIZED position, mirrored
in `Assets/Shaders/Trees/tree_record.inc.glsl` (not read by a shader yet). A member of TreeSystem, updated before its
`Enabled` check: it runs with the preview off.

* **Ring:** the terrain's (`ringRadius()`, `chunkSize()`, `generatedBounds()`), one chunk of hysteresis on eviction.
  On a camera chunk change the main thread requests the missing chunks and re-sorts the queue nearest first; Low pump
  jobs (`Gen jobs`, 2) take the front, with lazy staleness as the terrain pumps.
* **Pure function** (`placeChunk`): one `sampleGrid` at Full detail, ~8 m step + halo, then one candidate per cell of
  a `Candidate cell (m)` (5) lattice, jittered over the cell. Per species (`Placement`) its CLIMATE FIT: 1 inside its
  IDEAL box (`Temperature` / `Precipitation` min max, normalized by `temperatureTo01` / `precipTo01`), outside a Gaussian of the distance
  to the box (per axis, sigma `ClimateWidth`), faded to 0 between `Climate fade start` and `end` (0.10 / 0.25 of the
  peak), 0 outside its slope / altitude band. (Until 2026-10-04 an attractor POINT: the density peaked at one climate
  and every climate around it thinned - a precipitation band where a minimum was meant.)
  Each species' density at the candidate: Density x fit x its cluster fbm x (fit / the BEST fit there) ^
  `Climate sharpness` (4). The candidate exists with probability (the summed densities) x the cell's hectares x
  `Density scale` (so at most one tree per cell: 400 / ha at 5 m); its species is picked by share. Species that share a
  climate ADD: a rare species in a dense one's box (Willow in Oak's) takes nothing away from it.
  (History: until 2026-10-04 the pick was by Density x fit with the Gaussian cut at 2 % - a dense species' tail
  outnumbered a sparse one's core, pines among the acacias, and lone trees far from any forest; until 2026-10-05 by
  fit ^ sharpness normalized by the SUM - an equal-fit species halved the other's density.) Bushes are not stored.
* **A new generation** (everything dropped, every chunk regenerated) on: Enabled / Seed / cell / density tweaks,
  `Reload species`, a new sampler, a chunk size or bounds change. The generation counter is monotonic, so a late pump
  result of an old config never merges.
* **GPU (W2):** each merged chunk joins an upload queue; `Upload KB per frame` (1024) of records per frame go to the
  renderer's pool (`Renderer::addTreeRecordChunk`, RendererVK "WORLD TREE RECORDS"; `GPU pool (MB)`, 128, fixed - a
  full pool refuses chunks and warns once), each with its GROUND: `placeChunk` also writes the chunk's 17² heights
  (16 m at 256 m chunks, bilinear in its Full 8 m grid, 16-bit above the minimum - TreeRecordPool's encoding), the
  ground the far volume stands the records' trees on (the camera-centred height map's far cascade, ~100 m texels,
  buried them on every peak). Every chunk uploads, trees or not. An evicted chunk's GPU range is freed (deferred
  reuse). Disabling frees the pool; a ring radius change restarts (the pool's chunk map is sized by it).
* **CPU keep radius (chunks)** (8): the CPU keeps a chunk's records only inside it (world mode's expansion reads
  them; at least its near radius + 1). Beyond it they are dropped once uploaded; a chunk coming back inside is generated again (same
  records - nothing re-uploads).
* `Log stats`: chunks (in flight, queued, to upload), trees, per chunk average / max; CPU and GPU MB; per species of the
  CPU-held records.
* Placeholder ideal climates: Oak 9..18 C / 900..2200 mm, 90 / ha; Pine -5..5 C / 600..2200 mm, 90 / ha; Acacia
  20..30 C / 300..900 mm, 12 / ha; Willow as Oak, 4 / ha; all `ClimateWidth` 0.06.
* **ROCK RECORDS** (R4 of Docs/RockRenderingPlan.md, 2026-10-05, user-untested; `setRocks` - on with "Rocks/Enabled" +
  "Rocks/World/Enabled", handed over by TreeSystem every frame; a change is a new generation): the same 4-byte
  records in the same chunks. The record TYPES: the `.tree` list, then every `.rock` (name-sorted) from
  `firstRockType()` on - TreeWorld reads the `.rock` `Placement` blocks itself. `placeChunk` places the rocks FIRST:
  * **A rock type's lattice is in WORLD space** (`RockSpecies::cell` = 1.5 x its largest `Scale`, at least 6 m;
    anchored at the world origin, one rock per cell at most): a cell's rock is a pure function of (seed, type, cell),
    so a chunk computes the rocks of its NEIGHBOURS exactly as they do. It evaluates every cell that can put a rock
    within `ROCK_REACH` (11 m: half of `ROCK_MAX_SIZE`) of itself; its own rocks become records, the others only keep its trees out.
    For that the sample grid has a halo of `GRID_HALO` = 5 points (40 m: the rock's reach, the talus probe and the
    rugged measure's 2 cells) WHILE ROCK TYPES ARE PLACED - 43^2 samples per 256 m chunk, +51 % over the trees-only
    halo of 1 point (35^2), which a world without rocks keeps - plus the valley measure's coarse grid, 21^2 more.
    **The user does not mind chunk-generation cost much (2026-10-05); the per-frame cost is what counts** - all of
    this is in the pump jobs.
  * **A type has any number of RULES** (`RockSpecies::rules`: its `Placement` blocks) and **their densities ADD** -
    a rule for everywhere plus one for a climate the type is common in, each with its own ground (2026-10-05, the
    user: per-type placement, and climate controls - "the flat rocks common in the savanna with the acacias":
    `Slab.rock`'s second block). A rule's fit at the rock's (quantized) record position: its optional climate box (as
    a tree's), its `Altitude` band, x its `Slope` band (the slope UNDER the rock; soft edges with four values,
    `bandWeight` - a boulder rolls off a slope), x `mix(1, crag, Crag)` (the terrain shader's rock coverage - steep,
    or far above the macro altitude - at the terrain's DEFAULT texture tweaks: `CRAG_*`, a deliberate approximation),
    x `mix(1, talus, Talus)` (gentle ground with a steep slope `TALUS_PROBE` = 6 m uphill of it), x THE GROUND AROUND
    IT (below), x its cluster fbm (a noise per rule). The rock exists with probability (the rules' summed Density x
    fit) x the cell's hectares x "Rocks/World/Density scale". The record does not keep the rule: what a rule cannot
    set (`Scale`, `Sink`, `Align`, the shape) is the type's.
  * **THE GROUND AROUND A ROCK** (per rule; user-untested; the user, 2026-10-05: large types off the plains, to the
    cliffs and the hills; then "way more common in valleys, and also at cliff bottoms - rugged weighted to the lower
    end of the height"; then "boulders shouldn't sit on sloped terrain as much, they would roll off"). The slope
    under the rock cannot tell any of it (and with a `Slope` band below ~0.9 the crag fit's slope term is always 0,
    so a type spread evenly over the plains). ground = `mix(Plains, mix(Rugged high, Rugged low, lowEnd), ruggedness)`,
    then - when the rule has a `Valley` - `mix(ground, Valley, valley)`: IN A VALLEY THE MULTIPLIER IS `Valley`,
    whatever the others say (an override, not a product: Plains 0.3 x a product would need a Valley of 20 for a flat
    valley floor, and that x Rugged at a valley's cliff). Three measures, each only when a rule uses it:
    * `ruggedness` = smoothstep("Rocks/World/Rugged slope start", "full", the STEEPEST grid cell within
      `RUGGED_CELLS` = 2 cells of the rock) - 0.08..0.4 rise / run, 16-24 m. Each grid cell's slope, a separable
      square max (`windowRange`), read between the cell centres (`gridAt`).
    * `lowEnd` = smoothstep(0.25, 0.75, how far DOWN the rock sits between the highest and the lowest grid point
      within the same 2 points): ~1 at a cliff's bottom, ~0 on its top, 0.5 on an even hillside.
    * `valley` = smoothstep(`VALLEY_LOW_START` 0.5, `VALLEY_LOW_FULL` 0.9, the same over ~160 m) x
      smoothstep("Rocks/World/Valley relief start (m)", "full", that height range) - 15..60 m: LOW ground with higher
      ground within ~160 m - a valley's floor (flat ones too, up to ~300 m wide), the foot of a mountain. From its
      own COARSE grid (`VALLEY_STEP` 32 m, the lowest / highest point within `VALLEY_CELLS` = 5, a halo of 6: 21^2
      Full samples; on the chunks' common lattice, so the neighbours agree). The sampler's macro `altitude` cannot
      serve: it is the coarse stage's 7.68 km / px surface.
    `Plains 0` = never on flat ground (the Block). A first version was a WORLD rule by the rock's size
    (large rocks x0.1 on plains, x3 on rugged ground): removed, the user wants the control per type, not a smaller
    average size.
  * **FOREST** (per rule, `Forest open forest`; 2026-10-07, for dead wood): the density x mix(open, forest, the
    trees' own expected density there / `ROCK_FOREST_FULL` (60 per ha), clamped) - `speciesWeights`, the tree pass's
    own sum (every species' climate fit, faded, sharpened by the best fit, x its density and cluster patches, x "Trees/
    World/Density scale"), evaluated at the rock. The rocks are placed before the trees, so the measure is what the
    trees WILL be there, not the trees placed. `Forest 0 1`: only in forests, never in their clearings.
  * "Rocks/Reload types" makes TreeWorld read the Placement blocks again too (`RockSystem::typesRevision` through
    `setRocks`); "Trees/World/Reload species" does it WITHOUT regenerating the rock meshes - the fast way to iterate
    a Placement block.
  * **Rocks do not give way to each other** (boulders lie against boulders; a rejection order would chain across
    chunk borders). **The trees give way to every rock**: a tree candidate inside its FOOTPRINT is dropped - a
    CAPSULE along the record's yaw (hash 104, rock-local X turned to (cos, -sin) as the expansion turns it), its half
    length and radius `rockFootprint(type)` x `rockRecordScale(seed, Scale)`: a round rock's has no length and a radius
    of half its size (tree out inside 0.9 x that + 0.5 m, as before); a lying trunk's runs along the log with its
    thickness (+ 0.9 m: a disc over a 15 m log cleared the forest around it); a standing one's covers its base and
    roots. The clutter's floor map uses the same capsules (`FloorType::footprint`).
  * With rocks off the tree records are as before (the same hashes, the same field interpolation).
  * The far volume takes the rock records too (R5): see "World mode", THE WORLD'S ROCKS.

## World mode (TreeSystem, W3)

`Trees/Enabled` + `Trees/World/Enabled` + `GPU expansion` (+ terrain): the trees are TreeWorld's records, not the
preview grove. Switching it, or a TreeWorld restart (its `generation()`), respawns.

* **One DYNAMIC set** (RendererVK `createDynamicTreeInstanceSet`: every piece type, a fixed `Set capacity (pieces)`
  of slots, 600 k; no growth, a full set refuses chunks and warns once), hooked into the terrain like the preview
  (`hookTerrain`; chunk index capacity (2 x near + 3)^2 x 2).
* **Near radius (chunks)** (6, Chebyshev): every chunk inside it that TreeWorld holds CPU records for (TreeWorld's
  keep radius is raised to near + 1: `requireKeepRadius`) is EXPANDED on a Low job, nearest ring first, at most 4 in
  flight: per record its tree (`treeRecordSeed`: the preview's variant / scale / yaw rules, `Size variation`) on the
  ground of ONE Full `sampleGrid` at 2 m (the terrain's LOD 0 spacing) over the chunk plus the bushes' reach, plus its
  BUSHES (`Bushes per tree`, out to 0.75 x `Spacing`, the bush species of its climate - the preview's rule). The job
  reads only an immutable `ExpandContext` (types, scales, far centre / radius per variant, the record-type -> species
  map by name, the sampler) and a copy of the records.
* `Expand per frame` (2) finished chunks join the set per frame (`addTreeInstanceChunk`), the coordinate is mapped
  (`m_vegChunkOf`) and re-stamped in the terrain (`restampVegetation`). A chunk past near + 1 is removed
  (`removeTreeInstanceChunk`, deferred reuse) and re-stamped; a pending one's result is dropped.
* A respawn joins the jobs (`stopExpansion`: the generation bumps, the results are cleared).
* **THE WORLD'S ROCKS** (R4, 2026-10-05, user-untested): `spawnWorld` appends RockSystem's loaded types
  (`Globals::rocks.worldTypes()`) to the set - a GPU type per variant: ONE mesh, the LOD chain's level 0 in the `bark`
  slot on `LitRock`, no billboard (RendererVK: the culls pick its level, no wind) - and maps TreeWorld's rock record
  types to them by name. `expandChunk` turns a rock record into ONE piece (`placeRock`): variant and yaw from the seed
  as a tree's, the scale from `rockRecordScale`, laid ON THE GROUND by `RockSystem::groundTransform` (Rocks, "A rock on
  the ground": it follows the ground's normal by `Align`). The
  set's types are fixed, so RockSystem's `worldGeneration()` (its meshes arrived, changed or went - it loads in the
  background) respawns the set; and before RockSystem frees its meshes it calls TreeSystem's mesh-user hook
  (`setMeshUser`), which destroys the set.
  **ROCKS IN THE FAR VOLUME** (R5, 2026-10-06, user-untested): rock meshes hand over to the far-tree volume at its start
  as a tree's billboard (RendererVK tree_cull.inc.glsl: a type without a billboard drops its mesh past `Far start` +
  `Far overlap` in the main pass; it keeps casting its sun shadow). A rock GPU type carries its variant's OCCUPANCY grid
  (`RockVariant::density`: `ROCK_DENSITY_RES` = 16^3 over its rock-local box, the field soft over one voxel; generated
  with the meshes) as the type's `density`, with `solid = true` and the SINK baked into the box (`densityMin/Max` - Sink
  x height; the volume stands a piece on the ground; the lean is left out). `spawnWorld` hands the renderer a
  `TreeRecordTypeGpu` per rock record type too (albedo.w = 0: a SOLID): its variants as the set's rock types (max 8),
  its Scale range with no size variation (= `rockRecordScale`), per variant its occupied volume at scale 1, the profile
  over the sunk box. **Keep `placeRock`'s variant / scale / yaw hashes and the volume's record expansion in step**
  (102 / 103 / 104, as a tree's). RendererVK "Far-tree volume", ROCKS, has the volume side.
* **The far-tree volume (W4)** takes EVERY tree from the RECORDS (RendererVK "WORLD TREE RECORDS"), never from the
  set: within `Trees/Far record detail (m)` the GPU expands each record exactly as `expandChunk` (variant, scale, yaw,
  its bushes - the same hashes; the terrain map's ground) and splats it in detail; beyond, its exact mass per column.
  So a chunk entering or leaving the near radius changes nothing in the volume. The spawn hands the renderer the
  record types (`setTreeRecordTypes`: per TreeWorld type its variants as the set's types, Scale range, size
  variation, a tree's bush types in `ExpandSpecies::bushes` order, bushes per tree / radius, per-variant mass, the
  height profile); a respawn clears them. **Keep `expandChunk` and `tree_volume_splat.cs`'s `splatRecordPlant` /
  `main` (and `tree_volume_records.cs`'s variant / scale) in step.** At most 8 variants per species and 8 bush species
  per tree (a warning past that). Without `Trees/Enabled` (no species meshes, so no density) the records are not in
  the volume. It re-bakes at most every 30 frames while record chunks change.

## The preview (temporary)

`Trees/Enabled` loads every species (enable / `Reload species` re-reads the files — no hot reload yet),
uploads each piece as two `RenderMesh`es (bark + double-sided diamond leaf cards, solid-colour `LitOpaque`
materials), and spawns a `Grove size`² grove in front of the camera plus the piece library in rows behind it
(`Show piece library`; reloads - off, the module / trunk billboards are not uploaded). `Bushes per tree` (default 4; the fraction by chance) scatters the `Kind Bush` species
around each grove tree (1.5 m .. 0.75 × spacing out, area-uniform, a random bush species, the same variant / scale /
yaw rules). `Bush shadow distance (m)` (default 100, GPU path): bushes farther than this from the shadow
cascades' centre cast no sun shadow (`TreeInstanceType::shadowDistance`). `Grove type`: Mixed (the TREE species alternate) or one species by its `TreeSpecies` name — the
names are a fixed list in Settings.Trees (`TREE_GROVE_TYPES`; a tweak enum registers before the species load), so a
new species needs its name added there; a missing one falls back to mixed with a warning. Trees sit on a
`Spacing` grid (default 11 m; `Grove size` up to 512², default 350² of `Grove type` Oak, for the far-tree volume),
each offset by a seeded random `Position jitter` × spacing (default 0.8; 1 = anywhere in its cell;
the old fixed 0.3 left the rows visible), and scaled by the species' `Scale` range × `Size variation` (2^±v,
log-uniform, default 0.6 = ×0.66..1.52). One `RenderNode` per placed piece mesh, pushed every frame. **This path is replaced
by the dedicated GPU tree pipeline (G4: own shaders, per-tree bone palettes, own shadow draw); trees in that
path are not in the RT scene at first, but the design keeps RT addable.**

---

# Rocks

"Rocks" tweaks. **In development — the plan is `Docs/RockRenderingPlan.md`** (tiers, phases R1..R6). Status: R1
(2026-10-05; the shapes had a first round with the user): the `.rock` loader, the SDF generator and a PREVIEW; R2
(2026-10-05, user-untested): the climate rock material (RendererVK); R3 (2026-10-05, user-untested): a regular mesh
LOD chain + the per-vertex cavity (the tessellated route was dropped - the user's decision); R4 (2026-10-05,
user-untested): rocks in the WORLD - records in TreeWorld, pieces in TreeSystem's world set (see "World records" /
"World mode" under Trees); R5 (2026-10-06, user-untested): rocks in the FAR VOLUME (World mode, ROCKS IN THE FAR
VOLUME). "Rocks/Enabled" is on by default since 2026-10-06. Rocks are BIG objects (~1-22 m); small
stones are the GROUND CLUTTER (see "Ground clutter": its pebbles reuse the rock SDF generator). The type sets the SHAPE only; the colour comes
from the climate's terrain bedrock material (the rock material, R2). **DEAD WOOD** (2026-10-07, the user's request: "Dead /
fallen tree Rock types") is rock types too - `Shape Trunk` + `Surface Wood` ("DEAD WOOD" below): fallen logs, stumps
and snags through the same records, LOD chains, far volume and placement rules, wearing a tree species' bark texture
(the user, 2026-10-07: "just use a wood texture instead, can use an existing one" - no procedural wood noise).

## The model: an SDF per variant, meshed into a regular LOD chain

`buildRockShape(type, seed)` (RockGenerator.cpp) makes every random decision of a variant ONCE into a `RockShape`
(blocks, fracture planes, split plane, pits, strata / noise parameters), at a nominal size of 1 (the longest axis;
the instance scale makes metres). `rockSdf(shape, p)` is then a pure function: the point WARPED by low-frequency
noise (`Warp`: flat faces bulge, straight edges bend), the union of the superellipsoid blocks (smooth min, small k: a
pile's joints stay concave) - or, for `Shape Pillar`, the standing pillars (`pillarSdf`, "THE PILLAR" below) -, cut by the planes (smooth max, `Round` / 2), minus the split slab - all built `Erosion` SMALLER and the
field then grown by it (a Minkowski rounding: every convex edge and corner a radius-`Erosion` round, the size kept;
2026-10-05, the first shapes read as cubes) - plus the strata (grooves + a per-layer step), minus the fbm / ridged
noise, minus the pit spheres. CPU only: nothing evaluates the field on the GPU (GPU tessellation onto it was planned
and dropped, 2026-10-05). The field
lives in the SHAPE frame (body centred); the meshes in ROCK-LOCAL space, the lowest point at y = 0 (`originY`) - a
TRUNK's ground line instead (`RockShape::groundY`: a log's underside, a stump's base), its roots and root plate below.

`generateRockVariant`: surface nets over the shape's bounds (`Rocks/Grid resolution` cells along the longest
axis, default 32 - 96 for a finer source mesh; when the body reaches the grid's outer border - the bounds are a guess,
and the superellipsoid field is no exact distance, so a large `Erosion` on a thin axis grows the body past them - the
bounds grow by 15 % per side and it meshes again, up to 4 times: the mesh was OPEN at the border, the clutter's thin
pebbles, 2026-10-07; the erosion is also capped at 80 % of the body's thinnest half-axis), every vertex projected onto the field (2 Newton steps) and shaded
from its gradient: the FULL mesh, CPU working data only (never uploaded). Each vertex also takes its **CAVITY**
(`rockCavity`: five field taps out along the normal, Quilez' SDF occlusion - 1 = open, 0 = deep in a crevice) into
`texCoords.x`: a rock has no uv, so the u channel carries it to the rock vertex shader (AO, and where the ground
cover gathers). **THE LOD CHAIN** (`RockVariant::lods` / `lodError`, up to `ROCK_MAX_LODS` = 4): `meshopt_simplify`
of the full mesh to the type's `Lod` triangles (level 0), then a quarter per level down to
`ROCK_LOD_MIN_TRIANGLES` (24), each level from the FULL mesh (its error - in rock-local units, for RendererVK's
screen-space-error pick - is then against the true surface). The winding is fixed by the signed volume
(`orientOutward`: counter-clockwise outward). Every variant generates on its own job ("Rock generate").

## `.rock` — type asset (`Assets/Rocks/*.rock`, `loadRockType`)

```
RockType <name>
	Seed n · Scale min max (m, the longest axis; at most `ROCK_MAX_SIZE` = 22 m - the far volume's layer height,
	        the user's limit) · Variants n (default 6) · Sink (fraction of the height below ground; a lying trunk's
	        height is its thickness)
	Surface Rock|Wood (default Rock: the climate's bedrock. Wood: a tree species' bark texture - "DEAD WOOD" below)
	Bark <TreeSpecies> (Wood: whose bark texture it wears; default Oak) · Color r g b (Wood: a tint on that bark,
	        sRGB 0..1, default 1 1 1)
	Shape Boulder|Block|Pillar|Trunk (superellipsoid / superellipsoid / a STANDING body with a width profile, on a flat
	        floor / a dead TREE TRUNK - "DEAD WOOD" below)
	TRUNK: Lying 0|1 (1: along X, a fallen log; 0: along Y, a stump or a snag) · Aspect = the trunk's length along its
	        axis and its thickness across · Profile = its radius base to top · Squareness default 2 (round) ·
	        Break top base (splinter length x the trunk's radius at that end; one value: both; 0 = a worn end; a
	        standing trunk's base is cut flat into the ground) · RootPlate chance size (lying: an uprooted root plate
	        across the base, radius x the base radius) · Stubs count length (broken branch stubs, length x the
	        nominal size) · Roots count spread (standing: buttress roots, reach x the base radius) · Hollow chance
	        size (a hollow core, radius x the trunk's, open at the broken ends) · Resolution up to 8 (a log is thin:
	        its thickness needs the cells)
	Profile w0 w1 w2 w3 w4 (Pillar: its width at 5 even heights, base to top, a Catmull-Rom spline through them; the
	        widest = the Aspect's width. `1 0.8 0.6 0.38 0.1` a spire, `0.5 0.75 1 0.95 0.72` a top-heavy monolith)
	ProfileVar v (each width x (1 +- v) per variant and per pillar; default 0.15)
	Group count shrink (Pillar: 1..count pillars - the variant's draw - standing TOGETHER on one floor: each later one
	        ~x shrink, beside an earlier one, their bases overlapping; every one leans up to ~9 deg. Default 1)
	Squareness n (the superellipsoid exponent, 2 ellipsoid .. 8 near a box - for a Pillar its horizontal section;
	        default 2.2, Block 3 - a rounded box primitive read as cubes even with erosion, 2026-10-05)
	Aspect x y z (axis ratios) · AspectVar x y z (+-fraction per variant) · Round (fracture-plane edge rounding)
	Erosion (weathering: the radius every convex edge and corner is rounded to, 0..0.3, default 0.08)
	Warp amplitude frequency (low-frequency domain warp, default 0.04 1.5)
	Pile count shrink (Boulder / Block: 1..4 blocks HEAPED: each later block ~x shrink of the one before, turned and
	        tilted up to ~35 deg, from a random direction 5..60 deg above horizontal pushed in until it leans ~35 % of
	        its height into the earlier blocks; replaced the ordered `Stack`, 2026-10-05. No type uses it now)
	Fracture count depth (random plane cuts, each to (1 - depth x 0.3..1) of the support distance; 0..16. A Pillar
	        is never cut from below)
	Strata spacing depth var (horizontal grooves every `spacing`, `depth` deep, each layer stepped in / out x var)
	Noise amplitude frequency octaves · Ridged (0 fbm .. 1 ridged fbm)
	NoiseStretch s (the noise's features x s along Y: 2.5 = vertical flutes, 0.5 = flat layered lumps; default 1)
	Pits count size depth (spheres sunk into the surface from mostly upward directions; 0..32)
	Split chance gap (one crack through the rock, mostly vertical)
	Lod triangles (LOD 0's triangle count, 64..50000, default 2000; each further level a quarter of the one before)
	Resolution x (x "Rocks/Grid resolution" for this type, 0.5..4: thin features - strata, a spire's tip - need the
	        cells; generation time only)
	Align (0 upright .. 1 follows the ground's normal; default 1 - the preview reads it too)
	Placement (a world RULE - "ROCK RECORDS" under Trees. ANY NUMBER of blocks: their densities ADD, so a type gets
	        a rule for everywhere plus one per climate it is common in. A block without a Density is dropped)
		Density (per ha at full fit)
		Temperature min max (C) · Precipitation min max (mm/yr) · ClimateWidth (the rule's ideal climate box, as a
		        .tree's; default: every climate. The savanna = the Acacia's box, 20 30 / 300 900)
		Slope min max (rise / run UNDER the rock, a hard band) or Slope none full full none (soft edges: none below
		        the first, full between the middle two, none above the last - a boulder rolls off a slope:
		        `Slope 0 0 0.12 0.5`)
		Altitude min max (m above the local water)
		Crag (0..1: only where the terrain shows bedrock) · Talus (0..1: only at the foot of a steep slope)
		THE GROUND AROUND THE ROCK - density multipliers, default 1 ("ROCK RECORDS" has the measures):
		Plains x (flat ground: nothing steep within ~20 m, "Rocks/World/Rugged slope start / full". 0: never there)
		Rugged low high (rugged ground: at the LOW end of the heights within ~20 m - a cliff's bottom, the foot of
		        a hillside - and at their HIGH end - a cliff's top, a crest. One value: both)
		Valley x (low ground with higher ground within ~160 m - a valley's floor, the foot of a mountain;
		        "Rocks/World/Valley relief start / full (m)". IN a valley the multiplier is x, whatever Plains and
		        Rugged say. Default: no valley rule)
		Forest open forest (x mix(open, forest, the trees' own density there / 60 per ha): dead wood lies under trees.
		        One value: both; default 1 1)
		Cluster size coverage (m, 0..1: patches)
```

Every length is a fraction of the nominal size 1. Placeholders: `Boulder`, `Block` (a rounded block, 6 fractures), `Slab`
(flat, strata; common in the savanna), and two `Shape Pillar` types from the user's reference photos (2026-10-06,
user-untested): `Pinnacle` (limestone spires, 1-4.5 m, alone or up to 3 merged at the base, vertical flutes; a hot
desert, flat ground, dense fields; `Align 0`) and `Monolith` (a weathering pillar, 10-22 m: a narrow foot, widest
above the middle, deep strata, a blocky section; flat-ish ground only, away from rugged ground (`Rugged 0`, `Valley
0`), a few together and then none for a long way;
`Align 0`, `Resolution 2.5`, `Lod 6000`). They REPLACED `Column` (a columnar-basalt outcrop of packed hexagonal
prisms: `Shape Column`, `Columns`, `columnsSdf` - removed) and `Tor` (a pile of 3 blocks).

**THE PILLAR** (`pillarSdf`, RockGenerator.cpp): in its block's frame a superellipse section whose width follows the
profile spline over the height; first-order distance (r - width) / |gradient|. It has no bottom: it runs on below its
block and ONE horizontal floor cuts the whole group (`RockShape::floorY`, on the UNWARPED point in `bodySdf` - a
leaning pillar and the warp cannot open a gap under a foot), so the mesh's lowest point is that flat floor and `Sink`
only has to cover the ground's own unevenness. The top is the block's top plane. Erosion: the width - e, the top - e,
the floor + e, then the field grown by e (a spire's tip ends as a round of radius ~e).

## DEAD WOOD (`Shape Trunk`, `Surface Wood`; 2026-10-07)

Placeholders: `FallenLog` (4.8-14.4 m, lying, snapped or uprooted - a root plate in 35 % - stubs, sometimes hollow; `Bark
Oak`), the SNAPPED pieces of a fallen tree, 2.4-7.2 m (the user found the full logs too common, 2026-10-07 - FallenLog
density 8 -> 4, each piece 3): `SnappedLog` (its BASE HALF - FallenLog's thickness at half the length - both ends long
splintered breaks, `Break 4 4`, no root plate) and `SnappedTop` (its UPPER HALF - the top half's taper, more stubs -
`Break 2 4`: the snap at its base, the crown's break at its top), `Stump` (its roots' span 1-2.2 m, hollow in 30 %; `Bark Oak`), `Snag` (a standing dead tree, 5-14 m, its top
broken; `Bark Pine`, tinted cool); all `Forest 0 1`, in patches.

* **THE FIELD** (`trunkSdf` / `trunkBodySdf`, RockGenerator.cpp; the random decisions in `buildTrunk`): block 0 is the
  trunk with its axis along the block's Y - turned onto the shape's X when it lies (shape Y -> block -X: the block's x
  half is the vertical thickness), leaning up to ~5 degrees when it stands. Its side is the pillar's (`pillarSdf`: the
  profile as its radius). Its ends: BROKEN - the end surface `breakDepth` x the end's radius deep where `splinter()`
  is 0, at the end where it is 1 (ridged noise over the section: long splinters; a random lean across the section) -
  or worn (no depth: a cap rounded by the erosion); a standing trunk's base is cut flat (in the ground). The end terms
  are divided by their steepness (1 + 3 x depth): the field stays near a distance for the projection. The HOLLOW core
  (`Hollow`) runs the whole axis. Then, blended in with a fillet as wide as each: the LIMBS - round cones (iq's
  sdRoundCone): branch stubs along the upper trunk leaning toward the top (a lying trunk's within 126 degrees of up,
  never into the ground), buttress roots out of a standing trunk's base and down into the ground - and a lying
  trunk's ROOT PLATE (a lumpy superellipsoid disc across its base, the trunk entering its upper part). The box the
  normalization takes includes the limbs and the plate, so `Scale` is the longest extent of the whole (a stump's is
  its roots' span). The pits (knot holes) cast from the trunk's axis.
* **THE GROUND LINE** (`RockShape::groundY` = the mesh's y = 0): a log's underside, a standing trunk's base plane -
  the root plate's lower half and the roots go into the ground. A lying trunk's `height` (the sink's base) is its
  thickness, not its root plate.
* **THE WOOD DATA** (`woodCoords`, per vertex of the full mesh, carried through the simplification): the part the
  vertex lies on (the trunk, the nearest limb, or the root plate - by the smallest distance), its coordinates along
  and across that part - into the mesh's TANGENT; along in units of the part's BASE CIRCUMFERENCE (the bark
  texture's v, as on a tree branch; each limb offset), across in nominal units - and its BARK cover (smoothstep(0.68,
  0.86, the distance from the part's axis / its radius there): the outer skin is bark, the cuts, the hollow and a
  splinter's inner face are not) into texCoords.y as 1 + it. The rock shader's wood path reads both (RendererVK "The
  rock material", DEAD WOOD).
* **THE BARK** is the type's MATERIAL (`RockSystem`: `Type::woodMaterial`): the `Bark` species' texture pair -
  `Assets/Local/Trees/Textures/<Bark>_bark.png` + `_bark_normal.png`, the files TreeSystem writes once; when missing,
  generated from `Trees/<Bark>.tree` (`generateBarkImages`, not saved: TreeSystem owns those files) - x the `Color`
  tint, mipped (`buildBarkMips`) and BC1 / BC5-compressed in a Low job per wood type (`loadBark`, kicked with the
  variant jobs, in the same counter), uploaded in `finishLoad`. Kept over a remesh, freed in `clearAll`. A rock type's
  instances keep the shared grey material (`WorldType::material`). The far volume takes a wood type as a solid in its
  own colour (`WorldType::woodAlbedo`: the tinted texture's linear mean).
* **THE GROUND IT COVERS** (`rockFootprint(desc)`, from the type - TreeWorld places records without the meshes): a
  CAPSULE along rock-local X (a lying trunk: its half length and thickness; a standing one: its base and roots'
  reach; any other rock: a disc of half its size), x the record's scale, turned by its yaw. The trees keep out of it
  (World records, ROCK RECORDS); the clutter's floor map treats dead wood as a TRUNK (FloorType kind 3: the trunk
  proximity around the capsule - mushrooms and fallen branches gather there - and occupied inside it), a rock as
  before. `RockFootprint::flat` (the widest horizontal extent / the longest axis) scales `groundTransform`'s footprint
  (a snag's is its thin base, not its height - this also halves Pinnacle's and Monolith's, which sank too deep on a
  slope).
* **WHERE** (`Forest`, ROCK RECORDS): the trees' own expected density at the record.

## A rock on the ground (`RockSystem::groundTransform`)

**ONE rule for the preview and the world** (`spawnPreview`, TreeSystem's `placeRock`), a pure static function - a
rock FOLLOWS THE GROUND, it does not stand upright (2026-10-05, the user's decision; `Align` was 0.3 by default and
the preview had yaw only). Input: the terrain height at the rock's centre and at four points `FOOTPRINT` (0.35) x its
size x the type's `RockFootprint::flat` (its widest horizontal extent / its longest axis: a snag's thin base, not its
height) out along x and z.

* **Orientation** = lean x yaw: the yaw about the rock's own up axis, then the turn from straight up to the
  footprint's normal (central differences of the four points) x `Align`.
* **Position**: the origin (the mesh's lowest point, y = 0) on the plane through the four points, lowered to the
  centre height in a dip and by the saddle term the plane cannot follow - no edge floats. Then sunk ALONG THE ROCK'S
  UP AXIS by `Sink` x its height, plus - for `Align` < 1 - what the part of the slope it does not follow lifts its
  downhill edge: r x tan(the angle left between its up axis and the normal), at most 2 r. At `Align` 0 that is the
  old "stand on the lowest ground under the footprint".
* The strata, the up-facing pits and a pile's stacking turn with the rock. A type that must stay vertical on a slope
  sets `Align` below 1 in its file (`Pinnacle` and `Monolith`: `Align 0`).

## The preview

`Rocks/Enabled` (default ON since 2026-10-06; it was off during R1-R4) loads every type and generates its variants - one Low job each, in the BACKGROUND
(`m_genInFlight` polled per frame; a synchronous generation stalled main for seconds in Debug) - then, once all are
done, uploads every level (raytraced) and registers each variant's GPU LOD chain (`Renderer::createMeshLodChain`;
freed before its meshes), and spawns one ROW per type in front of
the camera, one rock per variant (random scale in the type's range, random yaw, on the ground by `groundTransform`), as plain
`RenderNode`s on the chain's LEVEL 0, pushed every frame: **the cull picks each rock's level** (RendererVK "Mesh
LODs", the "LOD" tweaks - `Force LOD` shows one level, `Max error (px)` moves the switches). `Preview shading`: **Rock material** (default; RendererVK
`EPipelineIndex::LitRock` - the climate's terrain bedrock, snow, ground cover and contact band, "Rocks/Material"
tweaks; R2, see RendererVK CONTEXT "The rock material") or **Flat grey** (`LitOpaque`: the shape alone). `Show
preview` (default OFF since 2026-10-06) off: no rows, the world's rocks only. **The rows ignore every placement rule**
(they stand wherever the camera looks, a hillside too): judge placement with the preview off. **`Rocks/World`**: `Enabled` (default on: rock records in TreeWorld +
rock types in TreeSystem's world set - they need "Trees/World/Enabled", "Trees/Enabled" and the terrain too),
`Density scale`, `Rugged slope start / full` and `Valley relief start / full (m)` (what the types' `Plains` /
`Rugged` / `Valley` mean - `RockWorldDesc`, see ROCK RECORDS; every change regenerates the record chunks); the system serves the world through `worldRules()` /
`typesRevision()` / `worldTypes()` / `worldGeneration()` / `setMeshUser`. `Reload
types` re-reads the files (a new `typesRevision`); `Respawn preview` re-places in front of the camera; `Grid
resolution` regenerates the MESHES only (`clearMeshes` + `kickGeneration`: the types stay as read, so the revision and
TreeWorld's placement stay too; only the world generation changes). After the upload the variants' CPU meshes are
freed (`finishLoad`; the density grid and the shape numbers stay). RT
sees the rocks through the chain's one BLAS. (The first enables' device-lost crash, 2026-10-05, was the renderer's:
unzeroed BLAS address entries for not-raytraced meshes - RendererVK CONTEXT, `createMesh`.)

---

# Ground clutter

"Clutter" tweaks (Settings.Clutter). **The plan is `Docs/GroundClutterPlan.md`.** Built 2026-10-07: pebbles, fallen
branches, mushrooms and flowers. It REPLACED `ScatterSystem` (one CPU `RenderNode` per instance, imported `.oc`
models; removed the same day - the user's decision). **Nothing per object lives on the CPU**: RendererVK's
`ClutterPipeline` places every object on the GPU each frame (RendererVK CONTEXT "Ground clutter": a grid of patches,
a ranked candidate list per patch and type, kept by the type's density there). `ClutterSystem` feeds it two things:

* **THE TYPES** - every `Assets/Clutter/*.clutter` (name-sorted; `loadClutterType`, ClutterType.cpp), loaded on the
  first enabled frame and on `Reload types`. Their meshes generate on one Low job per type ("Clutter generate",
  ClutterGenerator.cpp; `Mesh resolution` regenerates them), then `Renderer::setClutterAssets` takes them once (GPU
  idle): **a GPU type per `Placement` block** (their densities ADD; at most `CLUTTER_MAX_TYPES` = 32 blocks), the
  meshes concatenated (at most 256). The CPU meshes are freed after. Disabling keeps them (an enable draws at once).
* **THE FOREST FLOOR MAP** - the trees' and rocks' RECORDS (TreeWorld, through `Globals::trees.world()`; the CPU holds
  them inside "Trees/World/CPU keep radius") splatted on a Low job ("Clutter floor map", `bakeFloor`) into
  `CLUTTER_FLOOR_DIM`^2 (384^2) rgba8 texels of 1 m around the camera, MAX-blended: **R canopy** (a tree's crown: 1
  inside 0.75 x its radius, none past 1.1 x), **G trunk proximity** (1 at the trunk, squared falloff to none at
  max(3 m, 0.8 x the crown)), **B rock proximity** (1 up to 0.85 x the rock's radius, none at 1.6 x + 1.5 m - the
  apron at its foot), **A occupied** (inside a trunk + 0.15 m or 0.85 x a rock's radius: nothing grows there). The
  record types' meaning comes from `TreeWorld::floorTypes()` (a tree's crown / trunk radius at its species' MEAN scale
  - `.tree` CrownRadius / TrunkRadius x (1 + Flare); a rock's footprint CAPSULE from `rockFootprint` x
  `rockRecordScale` along its yaw, as the world places it; DEAD WOOD - a `.rock` of `Surface Wood`, kind 3 - counts as a
  trunk: the trunk proximity around its capsule, occupied inside it, no rock proximity).
  Re-baked when the camera leaves `Floor map/Rebake distance` (40 m; capped so the map still covers the clutter range)
  from its centre, when the CPU records under it change (a signature over the chunks and their counts) or on a
  TreeWorld restart; one bake at a time, handed over by `Renderer::setClutterFloorMap`. Without "Trees/World/Enabled"
  no map (the floor measures read 0). The GRASS reads it too: "Grass/Cover/Canopy thinning" (0.3).
* Main thread, after `trees.update` and `rocks.update`. Headless: never initialized.

## `.clutter` - type asset (`Assets/Clutter/*.clutter`, `loadClutterType`)

```
ClutterType <name>
	Kind Pebble|Branch|Mushroom|Flower
	Seed n · Variants n (meshes; not flowers) · Scale min max (x the nominal size 1: m for a pebble's longest axis, a
	        branch's length, a mushroom's height; for a flower x its Stem and HeadSize)
	Range m (from the camera; x "Clutter/Range scale"; the patch grid caps it, ~156 m at 4 m patches)
	Sink f (fraction of the height below the ground) · Align f (0 upright .. 1 follows the ground's normal; flowers: upright)
	Color r g b · Color2 r g b (sRGB 0..1) · Roughness f · Lod triangles (pebble level 0; a quarter per level)
	PEBBLE:   every .rock SHAPE key (Shape, Aspect, AspectVar, Squareness, Erosion, Warp, Fracture, Strata, Noise,
	          Ridged, Pits, Split - `readRockShapeKeys`): a small rock through the rock SDF generator. Color TINTS the
	          climate's bedrock (the rock material's pick)
	BRANCH:   Thickness (radius / length at the thick end) · Twigs n (0..6, lying flat) · Bend (degrees). Color = bark,
	          Color2 = end grain
	MUSHROOM: Cap Dome|Flat|Cone|Funnel · CapSize radius height (x its height) · StemRadius · Group n (1..5 per variant,
	          each smaller, leaning out) · Spots 0..1 (white spots on the cap). Color = cap, Color2 = stem and gills
	FLOWER:   Head Radial|Spike|Umbel|Bell · Petals n · Open degrees (Radial: the petals' tilt up; below 0 swept back) ·
	          PetalWidth (x HeadSize) · Stem m · HeadSize m. Color = petals, Color2 = the centre. NO MESH: the
	          vertex shader builds it (RendererVK "Ground clutter", the flowers)
	Placement (ANY NUMBER: each is its own GPU type, their densities add)
		Density (per m^2 at full fit) · Temperature min max (C) · Precipitation min max (mm/yr) · ClimateWidth
		Cluster size coverage (m, 0..1: drifts / groups) · Slope max (rise / run) · Altitude min (m above the water)
		THE TERMS - each `a b`: the density x mix(a, b, its measure 0..1); one value = both; default 1 1 (no matter):
		Grass bare full (the GRASS AS DRAWN: the terrain's cover x the blades' clumps / bare spots x the canopy thinning;
		        every flower type has bare 0: no flower where no grass) · Crag none full (bedrock showing) · Beach none full ·
		Canopy open shaded (crowns overhead) · Trunk far near · Rock far near (a rock's foot) · Wet dry wet (humidity)
		Ring radius width cell chance (fairy rings: the density x 1 on a ring of radius +-30 %, falling off over width;
		        one ring per `cell` metres with `chance`. 0 radius = none)
```

Snow removes everything; so does the floor map's "occupied". Placeholders (2026-10-07): stones
`Pebble` (everywhere a little, many at a rock's foot, on bedrock and the beach), `Flint` (flat, broken: dry stony
ground); wood `Twig`, `Branch` (under the canopy only, more at the trunks); mushrooms `Bolete`, `FlyAgaric` (red,
spots, cool forest), `Chanterelle` (groups, damp), `InkCap` (meadows), `FairyRing` (rings in the grass); flowers
`Daisy`, `Poppy`, `Cornflower`, `Buttercup`, `Dandelion` (radial), `Lupine`, `Heather`, `Lavender` (spikes),
`Yarrow` (umbel), `Foxglove` (bells, forest edge), `Bluebell` (bells, UNDER the canopy), `Gazania` (savanna),
`Gentian` (alpine).

## The meshes (`generateClutterMeshes`)

Nominal size 1, y up, the lowest point at y = 0, `CLUTTER_LODS` = 3 levels each (`Renderer::ClutterMesh`;
`RendererVKLayout::ClutterVertexGpu`: position + AO, normal + PART):

* **Pebble** - `generateRockVariant` with the type's rock keys at `Mesh resolution` (20) x its `Resolution` cells: the
  rock chain's levels 0-2, its cavity as the AO. `finishGeneration` CHECKS every pebble mesh level (`countBadEdges`:
  edges used once = a hole, more than twice = a fold) and logs a warning per bad one. As of 2026-10-07 no holes; the
  coarse levels (~50 triangles) of a few variants keep 1-2 non-manifold edges from the simplification.
* **Branch** - a bent, tapering tube along X (7 / 5 / 3 sides, 10 / 5 / 3 rings), lying on the ground (the underside
  darker), its end grain capped (part 1); levels 0-1 add the twigs (thinner tubes drooping to the ground).
* **Mushroom** - per group member a lathe (12 / 7 / 5 segments): the stem (part 0, wider at the foot), the cap top
  (part 1; the cap shape's profile, its rim curled down) and the gills underneath (part 2).

---

# Build note

`Procedural`'s diffusion `.cpp` TUs override the global flags to `/fp:precise /wd5050` — **load-bearing;
see `Code/Procedural/CMakeLists.txt`.**
