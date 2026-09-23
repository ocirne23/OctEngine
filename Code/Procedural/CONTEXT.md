# Procedural

> Library documentation for `Code/Procedural`.
> Read [`.claude/CLAUDE.md`](../../.claude/CLAUDE.md) first — rules, building, style, dependency direction.

The procedural world layer: diffusion terrain, FFT ocean, scattering, terrain physics, and the
camera-centered bakes they share. Links RendererVK, File, Spatial and Physics (+ onnxruntime, zstd
PRIVATE).

## The four globals

| Global | Type | Per-frame call |
|---|---|---|
| `Globals::terrain` | `TerrainStreamer` | `update(renderer, camera)` |
| `Globals::terrainCollider` | `TerrainCollider` | `update(camera.position, terrain.activeClimateMaps())` |
| `Globals::ocean` | `OceanGenerator` | `update(renderer, camera, terrain.activeTerrainData(), terrain.seaLevel())` |
| `Globals::scatter` | `ScatterSystem` | `update(renderer, camera, terrain.activeClimateMaps())` |

main.cpp updates them **in that order**, after entity updates and before present
([main.cpp:803](../App/main.cpp#L803)).

All four sit in init_seg `OC_SEG_PROCEDURAL`, **the first globals to destruct** — their dtors free
renderer residency and collider bodies and may wait on in-flight jobs. **Order among them is undefined
and deliberately independent.**

`ocean.swellTravelAngle()` → `terrain.setFlowWindAngle` is wired once at init.

> **Terrain is DISABLED by default** (`m_enabled = false`) — it needs 2.28 GB of ONNX weights on disk.

## Disabled = PARKED

Each system's `update()` self-gates on its Enabled tweak — collider and scatter also on
`maps == nullptr` — with an idle latch.

1. The enable → disable transition clears ONCE: residents, tiles and sectors freed, requests starved,
   renderer state pushed once (terrain params 0 / ocean params `enabled=false` / cleared terrain-data
   map).
2. It then keeps **DRAINING what is still in flight** — late pump results, the terrain-data bake, the
   collider build — **scope-free** until nothing is left.
3. After that a parked `update()` is a branch and a return: **no "Terrain" / "Terrain collider" /
   "Ocean" / "Scatter" profile scopes, no renderer calls.**

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

`temperatureTo01` / `precipTo01` map into the normalized space **the scatter rules' climate attractors
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
  thread-safe, and `sampleHeight` is called from the terrain streamer worker, the scatter worker AND
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
scatter cost nothing past the playable area, and the streamer's ring scan skips chunks that do not
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

* A bounded LOD ring around the camera: `ringRadius` 32 chunks, `chunkSize` 1024, `lod0Res` 512,
  `maxLod` 4, `lodStep` 0.3 chunks for the LOD0 band (each next band twice as wide, geometric),
  `fullResDist` 0.3, `skirtDepth` 5, `maxUploadsPerFrame` 16.
* Generated on up to "Gen jobs" **self-continuing Low-priority pump jobs** (`kickPump` CAS-claim +
  exit-recheck protocol, nearest-first). **`createMeshScene` runs IN the pump** — it is pure
  per-instance copying, audited — and the main thread only does the GPU-facing
  `ObjectContainer::initialize`, **so warm-tile mesh builds overlap cold V3 waits, which park fibers.**
* **Pre-emption points** (see Threading): the pump yields to higher-priority jobs between chunks,
  `generateChunk` after its `sampleGrid` and per vertex row, and `TerrainGenV3::sampleGrid` per row.
  **All of them sit OUTSIDE the V3 pipeline `JobMutex`** — the Normal collider job samples terrain
  and would park on that lock behind the very job holding it, so a point under the lock deadlocks.
  A cold-tile inference itself is never interrupted; it parks the fiber anyway.
* A `Resident` declares its `container` FIRST so it is destroyed AFTER the `node` that references its
  meshes. A chunk leaving the ring frees all GPU residency through `~ObjectContainer` →
  `Renderer::removeObjectContainer`, **so residency stays bounded across a session.**
* Each resident registers a `SpatialEntry` on `SpatialLayer_Terrain` with **`spawnVisible = false`** —
  chunks stream in off-screen constantly, and the guard would pin each one in the main pass until it
  first entered the frustum. Its userData is the **chunk key + 1** (0 would read as "dead" to the
  hand-over below, and key 0 is a real chunk).
* **The render push never walks the ring** (thousands of residents, a handful on screen). Two sets:
  1. **Main-visible chunks** come from the cull job's Main stamp through the Spatial visible-set
     hand-over, collect slot 1 (`setVisibleCollect(SpatialLayer_Terrain, 1)` in `initialize`,
     `visibleHandles(1)` in `update`) → `PASS_ALL`.
  2. **Main-culled chunks keep shadow + GI** (the ground behind the camera must stay in the TLAS and
     the sun cascades), but only inside a `forEachInSphere` around the renderer's scene focus with
     radius `max(shadow maxDistance + casterPad, RT/TLAS Range)` — the range past which the GPU
     shadow cull and the TLAS range bound drop the push anyway; farther ground gets its sun shadow
     from the terrain march over the baked height map. `MainOnly` skips this set; a main-stamped hit
     is skipped here (already pushed). Culling `Off` keeps the plain walk over every resident.

  **The pushes run on a worker** (`"terrainRenderPush"`, High, `m_renderCounter`): `update` resolves
  the hand-over handles to node pointers on main (unlocked pool reads; the scatter registers on main
  meanwhile) and submits the job; main.cpp calls `joinRender()` right before `present`, and `update`
  / `clearResidents` join it before they touch `m_residents` (a node container, so the pointers hold).
* **Eviction does not walk the ring either.** The unwanted residents (column outside the ring, or
  wanting another LOD) are a function of the ring and the resident set only, so `m_evictCandidates`
  is rebuilt by one walk when `ringMoved` or a chunk uploaded; every frame checks only the candidates
  against the stamps for the hole-free handover.
* **The ring scan is a job too** (`"terrainRingScan"`, Normal, `m_ringScanCounter`), kicked LAST in
  `update` — after the drain and the eviction, the frame's last writers of `m_residents` /
  `m_pending`, which the scan only reads — from a by-value snapshot (`m_ringScanIn`; the job
  captures `this` only, inline job storage is small). The NEXT `update` joins it first and applies
  `m_ringScanOut`: keys that became pending or resident meanwhile are skipped, the rest go pending,
  get published and kick the pump. One frame of request latency against seconds of generation; a
  request the ring moved away from is stale like any other and dropped by the pump.

  What stays on main: the config/model polling, the terrain/texture/wet param setters, the fog
  height-map handover (the bake itself is already a job — `HeightMapBaker::update` polls it), the
  result drain (`ObjectContainer::initialize` is GPU-facing) and the candidate eviction (a
  `~Resident` releases GPU residency into the renderer).

## It owns THE world datum

Handed out, never duplicated:

| Accessor | Consumer |
|---|---|
| `seaLevel()` | **The ocean floats on this rather than owning a second copy** — the generator builds its heights around it (moving it regenerates chunks), and the swash gate compares the two, so a fork switches the swash off everywhere. Valid even while terrain is disabled. |
| `activeClimateMaps()` | The collider, the scatter. **nullptr while terrain is disabled** — consumers treat that as "no terrain" rather than sampling a field that is not drawn. |
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

It also bakes the splat textures (diff / nor / arm, plus the `disp` height map as BC4 - optional per set,
the terrain relief's input: the parallax + height blend in "Terrain/Textures", the displacement in
"Terrain/Tessellation"; see the RendererVK CONTEXT), BC-compressed to `Assets/Local/TerrainTex` — a background job
kicked ONCE, at startup while the terrain is enabled or from `updateTerrainTextures` when it is
enabled later (`kickTexBake`; a disabled terrain never reads the source image sets); **the TERRAIN
shader falls back to flat colours until that bake finishes.** Each entry's climate box registers with
its textures (`TerrainSplatMaterial::climate`: temperature as t01, precipitation in mm/yr); only the
mm-per-full-humidity divisor stays live, pushed every frame as `TerrainTexTweaks::precipFullMm`.

The streamer also owns the **"Terrain/Water" tweaks** - ONE wetness field (rain, the ocean's swash,
submersion) driving ONE water surface: the field (texel size, update rate, diffusion, rain, dry time +
temperature sensitivity, wet-in, slope drain), the surface (fill start / full / curve = how far the wetness
fills the splat relief, the film max slope + slope fade that sink it on slopes, edge fade), the hand-over to the ocean (ocean blend = the film fading into the
ocean's water, ocean edge fade = the ocean blending out at its edge over the film) and the look (waviness, normal scale, wind ripples,
water (film) / wet ground / underwater ground roughness, wet darkening, darkening / roughness thresholds, wet normal scale, the drying pattern = dry islands from a world fBm: strength, darkening / roughness edge, size, relief share, contrast). They push every frame from `updateTerrainTextures` through
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
metre, and the sea is drawn at model × scale (1 = the model sea; **0.1 is the default**, a 10× sea against
the default terrain's 100× compression, tuned by eye rather than tied to the terrain's factor). Applied in ONE place, `pushOceanParams`, which fills the scaled `m_params` the renderer gets —
**and the CPU buoyancy mirror reads `m_params`, never the tweak members**, so the two cannot disagree
about the scale.

| Quantity | Scaling | Why |
|---|---|---|
| fetch, depth, cascade patch sizes | × s | Froude similarity: with gravity untouched this is the ONE scaling of the JONSWAP/TMA inputs under which wavelengths AND heights both come out × s — the scaled sea is a shrunk copy, not the full-size spectrum aliased into small patches. |
| wind speed | × √s | The velocity half of the same similarity (`U²/(F g)` and the wave-age ratio stay invariant). |
| every other metre (shore depths, cull slack, RT ranges, horizon offset, steer range, **ring cell**) | × s | Same world, fewer metres. The ring cell shrinking keeps the clipmap's detail per wavelength and its reach per model kilometre. |
| absorption, SSS strength (per metre) | ÷ s | The same water column in fewer metres, so deep water stays deep-coloured. |
| dimensionless ratios (amplitude, choppiness, the approach-band fraction, swash amplitude, foam thresholds) | — | The break acceleration is a fraction of g, invariant under Froude scaling. |
| sea level | — | The world datum, owned by the terrain. |

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
its hole, the horizon band as its 4 sides. **Each is a container/node with its own SpatialIndex entry**
(`SpatialLayer_Ocean`, userData = sector index + 1, `spawnVisible = false`), so the CPU visibility gate
and the GPU per-instance frustum cull drop off-screen water **instead of vertex-shading the whole
multi-km disc every frame.**

* All sectors share ONE transform, **snapped to a lattice multiple so vertices re-land on the same
  world positions** as the camera moves.
* **Both culls pad their bounds by `displacementExtent()` every frame.** The mesh they were built from
  is the UNDISPLACED lattice; the vertex shader then moves each vertex by the wave height and by the
  CHOPPY horizontal displacement, which scales with "Choppiness". A sector's bounding sphere absorbs
  some of that incidentally — its XZ half-diagonal exceeds its half-width — and **that spare slack is
  what a high choppiness runs out of**, dropping sectors with their crests still on screen. It shows at
  the SCREEN EDGES, where the frustum planes cut closest to a sector's own sphere.
  * `estimateWaveExtents` (the sparse readback re-scan that already found the trough) also returns the
    crest and the largest RAW horizontal displacement. **Raw on purpose**: the maps store Dx/Dz before
    the choppiness lambda, so the live tweak scales the padding with no re-scan.
  * The CPU side re-registers `baseRadius + extent` per frame; the GPU per-instance cull gets the same
    number through `u_oceanParams10.w` and adds it for `PIPELINE_IDX_OCEAN` instances
    (`instanced_indirect.cs.glsl`) — **frustum test only**, so LOD selection still sees real bounds.
* **Sector borders duplicate identical vertices, so splitting cannot open seams.**
* **Every triangle is emitted in BOTH windings** (`pushTri`), so the back-face-culled Ocean pipeline
  draws the surface from either side and the scene depth holds the nearest face from below too. (A
  cull-none variant shaded every wave crossing along an underwater ray — stacked "water planes".)
  Only the index count doubles. **So
  `gl_FrontFacing` is always true on the water**; the ocean shader takes the side from the triangle's
  plane instead.
* The Spatial Main gate skips off-screen sectors (ocean is `PASS_MAIN` only), and a "Dry sector cull"
  skips sectors fully buried AND inside the streamed-mesh radius.
* **`update` never walks the grid for the push, and mostly not at all.** The re-centering of the
  sector entries (transform + `updateEntry`, padded by `displacementExtent()`) runs only when the
  snapped position, the sea level or the pad changed — the snap steps by 8 cells and the pad
  refreshes every 15 frames — and stays on main (the scatter registers entries later in the frame;
  an `updateEntry` on a worker could race the pool growth). The main-visible sectors come from the
  Spatial hand-over, collect slot 2 (`setVisibleCollect(SpatialLayer_Ocean, 2)` in `initialize`,
  `visibleHandles(2)` resolved to `Sector*` on main); culling `Off` takes every sector.
* **The rest is a job** (`"oceanRenderPush"`, High, `m_renderCounter`, inputs snapshotted in
  `m_renderIn`): the dry test + `PASS_MAIN` pushes, the displacement readback copy (`m_dispTile`;
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

The dominant spectrum term travels AGAINST `windDirection`, so the swell heading is
`swellTravelAngle() = wind + π`. `steeredWindAngle` slews the SIM wind toward the baked shore flow so
waves roll inland.

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

"Ocean/RT" tweaks budget per-pixel scene rays.

---

# `TerrainCollider`

"Terrain/Collision" tweaks. A **focus-centered** ring of small static triangle-mesh collider tiles.

**Sampled from the SAME `ITerrainSampler` the terrain renders**, on the render LOD0 lattice with the
render mesh's triangulation, **so bodies rest exactly on the drawn surface.**

It **never touches the render chunks**: a LOD0 chunk is ~500k triangles, and a collider only needs the
ground near dynamic bodies. Tiles are a few tens of metres and exist only within "Radius" of the focus
(the camera — thrown bodies start there).

Each tile is `sampleGrid`'d and BVH-built on **ONE in-flight job, nearest-first**; the main thread only
creates and destroys the static bodies (layer "Terrain"), which is cheap in box3d. `createCollisionMesh`
is standalone and thread-safe, **which is what lets the build run off-main at all.**

Tiles clear and rebuild on a sampler identity change, exactly like the streamer's residents; eviction
has half-tile hysteresis. `maps == nullptr` clears everything. The dtor waits out the in-flight build
(main helps).

Verify with `Physics/Debug/Draw colliders`.

---

# `ScatterSystem`

"Scatter" tweaks. Trees, rocks and grass.

**Config is two function-local tables in Scattering.cpp**, `scatterAssets()` and `scatterRules()` —
assets are shared, so any number of rules can reference one by name.

## `ScatterAsset`

`ocPath` is an `.oc` file, **so the model path AND its import options (MergeNodes /
PreTransformVertices / DecimationFactor) come from it and scatter shares ONE cooked `.vsc` per model
with World.** Plus `nodes` (spawnable node paths — each placement picks one at random for variants;
empty = the model root), `footprintRadius` (ground exclusion; **0 = never blocks or is blocked — cheap
grass**), `minScale` / `maxScale`, `slopeAlign` (0 upright .. 1 fully following the terrain normal), and
`sinkDepth` (embedded below the surface, **hiding floating edges on slopes**).

## `ScatterRule`

**Climate ATTRACTORS in REAL units** — mean annual temperature °C and annual precipitation mm/yr, the
same units the terrain's texture table uses — with a Gaussian falloff of width `climateWidth`.

> **A rule says what climate it WANTS**, rather than pointing at a table entry that says it somewhere
> else. It named a biome enum once; that enum belonged to the old noise generator and went with it, and
> **the indirection was worth losing on its own.** Density falls off smoothly, so scatter borders are
> soft and track the ground they stand on instead of snapping at a classification boundary.

Plus `density` (instances per hectare at full climate weight), `viewDistance`, `clusterSize` /
`clusterCoverage` (patch feature size; 0 = even coverage), `maxSlope`, and a `minAltitude` /
`maxAltitude` band **above the local WATER level, which keeps things off beaches.**

## How it runs

Cell placements are computed **deterministically** on pump jobs — the same CAS-claim protocol,
"Scatter/Gen jobs" — from the SAME sampler the terrain renders: footprint dart-throw (larger footprints
placed first), cluster noise, slope and altitude bands.

**Per-rule `viewDistance` spawns and despawns instance groups WITHOUT regenerating cells.** Groups
register on `SpatialLayer_Terrain` with `spawnVisible = false`, register once (instances never move),
and everything regenerates on a sampler identity change.

> Like terrain chunks, scatter uses its **own layer so gameplay queries never see it** — its
> `userData` is 0, not an `Entity*`.

---

# Build note

`Procedural`'s diffusion `.cpp` TUs override the global flags to `/fp:precise /wd5050` — **load-bearing;
see `Code/Procedural/CMakeLists.txt`.**
