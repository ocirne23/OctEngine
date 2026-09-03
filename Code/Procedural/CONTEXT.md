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
* **The 2.28 GB of models load once per process and are RESEEDED, never reloaded.** So constructing a
  `TerrainGenV3` is cheap and a tweak rebuild costs nothing unless the seed actually moved. The load is
  DEFERRED until "Terrain/Enabled" — `rebuildMaps` early-outs disabled without constructing the
  generator, **and constructing it is the only load kick. There is no unload API**: disabling after a
  load keeps the models resident.
* **`isReady()` is a real state.** Until the models are downloaded and loaded it is false and the
  generator must not build geometry — **it would bake a flat world into the chunk cache.**
  TerrainStreamer polls it and rebuilds once it flips.

Tiles cache to disk under `Assets/Local/Diffusion/<seed>/` (zstd).

## World scale

**`metersPerPixel` is a UNIFORM scale**: it shrinks elevation and detail by the same factor as the
horizontal, **so a compressed world keeps the model's proportions** — same slopes, shapes and climate,
just smaller and quicker to fly across.

| Value | Meaning |
|---|---|
| **30** | The model's true training scale; continents are continent-sized and peaks ~10 km. A tile is then 7.68 km. |
| **3 (the default)** | A 10× compressed world; a tile is 768 m. |

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
* A `Resident` declares its `container` FIRST so it is destroyed AFTER the `node` that references its
  meshes. A chunk leaving the ring frees all GPU residency through `~ObjectContainer` →
  `Renderer::removeObjectContainer`, **so residency stays bounded across a session.**
* Each resident registers a `SpatialEntry` on `SpatialLayer_Terrain` with **`spawnVisible = false`** —
  chunks stream in off-screen constantly, and the guard would pin each one in the main pass until it
  first entered the frustum.

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

It also bakes the splat textures, BC-compressed to `Assets/Local/TerrainTex` at startup; **the TERRAIN
shader falls back to flat colours until that background bake finishes.**

---

# `HeightMapBaker`

The async snapshot bake driver: **ONE Low job in flight** (JobCounter-polled), re-baking on camera
drift past range/4 or a config change — **configs carry `operator==` so tweaks stay live**.

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
(`SpatialLayer_Terrain`, `spawnVisible = false`), so the CPU visibility gate and the GPU per-instance
frustum cull drop off-screen water **instead of vertex-shading the whole multi-km disc every frame.**

* All sectors share ONE transform, **snapped to a lattice multiple so vertices re-land on the same
  world positions** as the camera moves.
* **Sector borders duplicate identical vertices, so splitting cannot open seams.**
* The Spatial Main gate skips off-screen sectors (ocean is `PASS_MAIN` only), and a "Dry sector cull"
  skips sectors fully buried AND inside the streamed-mesh radius.
* **The horizon band is exempt from BOTH under-terrain culls** — a negated cell size flags its
  vertices, because its huge triangles break the vertex cull's footprint assumption.

## What runs where

Everything else is on the GPU: `OceanSimulationPipeline` (in RendererVK) simulates the spectrum and
IFFT into displacement and gradient maps **each frame, so every parameter is live**; the Ocean pipeline
variant displaces this mesh and shades it (RT refraction, Beer-Lambert). Params push through
`Renderer::setOceanParams`; **the mesh rebuilds only when ring params change.**

**The ocean bakes NOTHING itself** — shoaling, surf, swash and the land cull all read the streamer's
terrain-data map, and the GPU passes read the SAME bake the CPU copy comes from, **so the drawn water
and the simulated water agree by construction.**

## `sampleWaterHeight` — the buoyancy field

CPU-evaluated from the GPU displacement readback, and **a full CPU MIRROR of the clipmap vertex
shader**: cascade sum, shoaling fade, swash run-up and the waterline floor. ~2 frames latent —
invisible for physics. The App wires it into `PhysicsWorld::setWaterSurface`.

> **The shader is what you see, the mirror is what floats on it. Changing either without the other is a
> silent bug** — bodies sink through drawn waves, or float on dry sand.

It returns `-FLT_MAX` where there is no water: ocean disabled, readback not primed, or land beyond the
swash run-up band. **Inside that band it returns the live tongue surface, which SINKS BELOW the terrain
as the wave recedes (the drawdown floor) — so bodies beach themselves**, and callers want a plain
surface-vs-point test rather than a separate dry check.

`hasWater()` is the App's global gate for the buoyancy pass, **so a disabled ocean costs physics
nothing** — the pass otherwise sweeps the whole broadphase every step.

## Wind

The dominant spectrum term travels AGAINST `windDirection`, so the swell heading is
`swellTravelAngle() = wind + π`. `steeredWindAngle` slews the SIM wind toward the baked shore flow so
waves roll inland.

> Per-pixel domain rotation is **disabled** in `ocean_wave.inc.glsl` — it creases. Read the comment
> before reviving it.

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
