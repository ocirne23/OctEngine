# Procedural

> Library documentation for `Code/Procedural`.
> Read [`.claude/CLAUDE.md`](../../.claude/CLAUDE.md) first — rules, building, style, dependency direction.

The procedural world layer: diffusion terrain, FFT ocean, scattering, terrain physics, and the
camera-centered bakes they share.

## The four globals

| Global | Type |
|---|---|
| `Globals::terrain` | `TerrainStreamer` |
| `Globals::terrainCollider` | `TerrainCollider` |
| `Globals::ocean` | `OceanGenerator` |
| `Globals::scatter` | `ScatterSystem` |

All sit in init_seg `OC_SEG_PROCEDURAL`, the first globals to destruct — their dtors free renderer
residency and collider bodies and may wait on in-flight jobs. Order among them is undefined and
deliberately independent.

main.cpp updates all four per frame in the order terrain → collider → ocean → scatter, after entity
updates and before present.

`ocean.swellTravelAngle()` → `terrain.setFlowWindAngle` is wired once at init.

### Disabled = parked

Each system's `update()` self-gates on its Enabled tweak (collider and scatter also on
`maps == nullptr`) with an idle latch (`m_disabledIdle` / `m_inactiveIdle`).

* The enable → disable transition clears once: residents, tiles and sectors freed, requests starved,
  renderer state pushed once (terrain params 0 / ocean params `enabled=false` / cleared terrain-data
  map).
* It keeps DRAINING what is still in flight — late pump results, the terrain-data bake through
  `HeightMapBaker::inFlight()`, the collider build — scope-free until nothing is left.
* After that a parked `update()` is a branch and a return: no "Terrain" / "Terrain collider" /
  "Ocean" / "Scatter" profile scopes, no renderer calls.
* A config-dirty tweak while parked (e.g. sea level) re-runs the terrain drain once. The ocean frees
  its clipmap sectors on disable, and `rebuildGrid()` restores them on re-enable
  (`m_sectors.empty()`).

## `ITerrainSampler` (`:TerrainSampler`)

THE terrain field interface: pure functions of world (x, z) — seeded, thread-safe, world-continuous —
held as `shared_ptr<const>` so workers finish against old maps across rebuilds.

* `samplePoint` / `sampleGrid` fill a `TerrainPoint` (height, water level, temperature + sea-level
  baseline, humidity, fog, flow angle) in ONE evaluation.
* Wide-area consumers must use `sampleGrid`: it hoists per-point overhead, and for V3 it takes one
  lock per tile.
* `ESampleDetail::Coarse` for km-scale bakes.
* One `lapseRatePerMetre()` for the whole world.

## `TerrainGenV3` (`:GeneratorV3`, `Private/Diffusion/`)

ONNX diffusion pipeline: ONNX Runtime + DirectML, 2.28 GB of models loaded once per process,
reseeded but never reloaded.

* The load is DEFERRED until "Terrain/Enabled": `rebuildMaps` early-outs when disabled without
  constructing the generator, and constructing it is the only load kick. There is no unload API —
  disabling after a load keeps the models resident.
* 256×256 tiles (7.68 km at the native 30 m/px; `metersPerPixel` is a uniform world scale). A point
  read is a bilinear tile fetch.
* A cold tile takes ~1.5 s, with inference serialized by a fiber-parking `JobMutex`; concurrent
  requesters park on a per-tile `JobEvent`.
* Tiles cache to disk under `Assets/Local/Diffusion/<seed>/` (zstd).
* Sub-30 m relief is slope-masked noise (`:Noise` `fbmEroded`).
* `isReady()` gates everything until the models load, otherwise a flat world bakes into the caches.

## `TerrainStreamer` ("Terrain*" tweaks)

Render-only chunk streaming: a bounded LOD ring around the camera.

* Generated on up to "Gen jobs" self-continuing Low-priority pump jobs (`kickPump(n)` CAS-claim +
  exit-recheck protocol, nearest-first). `createMeshScene` runs IN the pump; main only initializes
  the container, so warm-tile mesh builds overlap cold V3 waits — those park fibers.
* Chunks leaving the ring free all GPU residency (`~ObjectContainer`).
* Owns THE world datum other systems consume, handed out and not duplicated: `seaLevel()`,
  `activeWaterReach()`, `activeFlowField()`, `activeClimateMaps()`, `activeTerrainData()`.
* Bakes the shared **terrain-data map** — 2 near/far cascades, RGBA32F: height, water level, packed
  fog|flow|temp|humidity → `Renderer::setFogTerrainHeightMap`. Consumed by fog terrain-follow, ocean
  depth/level (GPU through `terrain_height.inc.glsl` plus a CPU copy for buoyancy and wind steering),
  terrain colouring, and the terrain sun march.
* Bakes the splat textures (BC-baked to `Assets/Local/TerrainTex` at startup).

## `HeightMapBaker` (`:HeightMapBaker`)

The async snapshot bake driver, with `applyWaterReach` and `applyFlowField`.

* One Low job in flight (JobCounter-polled). Re-bakes on camera drift > range/4 or a config change —
  configs carry `operator==`, so tweaks stay live. The center snaps to the coarsest texel lattice.
* **WaterReach** — a chamfer distance-to-ocean sinks the baked water level under ground the ocean
  cannot reach, which keeps beaches off inland hollows.
* **FlowField** — 8-bit per-texel flow direction: toward land through the surf zone, downhill
  elsewhere. Baked into both maps' flow bits.

## `OceanGenerator` ("Ocean*" tweaks)

The CPU side of the FFT/Tessendorf ocean.

* Camera-following geometry clipmap: fixed cell size per ring, CDLOD morph, and a FLAT "Horizon band"
  ring extending to the far plane so the sea always meets the horizon.
* Split into SECTORS — each an ObjectContainer/RenderNode plus a SpatialEntry on
  `SpatialLayer_Terrain`, one shared snapped transform, duplicated border verts. The Spatial Main
  gate skips off-screen sectors (ocean is `PASS_MAIN` only), and a "Dry sector cull" skips sectors
  fully buried AND inside the streamed-mesh radius. The horizon band is exempt from BOTH
  under-terrain culls — negated cell size flags its vertices, since its huge triangles break the
  vertex cull's footprint assumption.
* The simulation (spectrum → IFFT → displacement/gradient maps, re-evaluated every frame so every
  param is live) is `OceanSimulationPipeline` in RendererVK.
* Bakes NOTHING itself — shoaling, surf, swash and land cull all read the streamer's terrain-data
  map.
* `sampleWaterHeight` is the CPU buoyancy field from the GPU displacement readback; App wires it into
  `PhysicsWorld::setWaterSurface`.

> `sampleDisplacement` is a full CPU MIRROR of `oceanSampleDisplacement` (`ocean_wave.inc.glsl`). The
> shader is what you see, the mirror is what floats on it — changing either without the other is a
> silent bug (bodies sink through drawn waves, or float on dry sand).

* It returns `-FLT_MAX` only past the run-up band; inside it the drawdown floor sinks the surface
  under the sand, which beaches bodies.
* **Wind** — the dominant spectrum term travels AGAINST `windDirection`, so the swell heading is
  `swellTravelAngle()` = wind + π. `steeredWindAngle` slews the SIM wind toward the baked shore flow
  so waves roll inland. Per-pixel domain rotation is disabled in `ocean_wave.inc.glsl` because it
  creases — read the comment before reviving it.
* "Ocean/RT" tweaks budget per-pixel scene rays.

## `TerrainCollider` ("Terrain/Collision" tweaks)

A camera-centered ring of small static triangle-mesh collider tiles — by default 32 m tiles within
96 m, at 1 m spacing = the render LOD0 lattice, so bodies rest on the drawn surface.

* Never touches the render chunks. Each tile is `sampleGrid`'d and BVH-built (`createCollisionMesh`,
  standalone and thread-safe) on ONE in-flight Low job, nearest-first; the main thread only creates
  and destroys the static bodies (layer "Terrain").
* Tiles clear on a sampler identity change. Eviction has half-tile hysteresis.
* Verify with Draw colliders.

## `ScatterSystem` ("Scatter" tweaks)

Trees, rocks and grass.

* Config is two function-local tables in Scattering.cpp: `scatterAssets()` — `.oc` models with
  footprint, scale and slope settings — and `scatterRules()` — climate ATTRACTORS in real units
  (°C, mm/yr) with Gaussian falloff. No biome enum.
* Cell placements are computed deterministically on pump jobs (the same claim protocol,
  "Scatter/Gen jobs") from the SAME sampler the terrain renders: footprint dart-throw, cluster noise,
  slope/altitude bands.
* Per-rule `viewDistance` spawns and despawns instance groups without regenerating cells.
* Everything regenerates on a sampler identity change.
