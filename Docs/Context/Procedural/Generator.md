# TerrainGenV3 — the diffusion generator

> Part of the Procedural documentation ([`Docs/Context/Procedural/CONTEXT.md`](CONTEXT.md)) — read that first.

`:GeneratorV3`, `Private/Diffusion/`. An ONNX Runtime + DirectML pipeline behind `ITerrainSampler`.

**Why the class looks the way it does:** an `ITerrainSampler` looks like a pure function of (x, z),
but this one generates **256×256-pixel TILES through three ONNX models.**

* **Sampling a point means bilinearly reading a resident tile.** A miss BLOCKS the caller while the
  tile is generated — **~1.5 s cold, ~0 once resident.**
* **All inference is serialized onto one lock.** *Required, not a convenience*: the tile store is not
  thread-safe, and the sampler is called from the terrain streamer's pumps, the tree record pumps, the collider AND
  the height-map baker. The lock is a fiber-parking `JobMutex` (`m_pipelineMutex`,
  [GeneratorV3.cpp:786](../../../Code/Procedural/Private/Diffusion/GeneratorV3.cpp#L786)) so a ~1.5 s cold-tile wait
  parks a FIBER rather than a pooled worker, and concurrent requesters of one tile park on its `JobEvent`.
  **The lock covers ONLY the pipeline call** (`get` / `getCoarseSlice`, plus reading the seed and
  precision it ran with): the plane assembly, the sea-level baseline recovery, the coarse lapse
  regression and the quantised zstd disk write are the tile's own CPU work and run UNLOCKED, so the
  next tile's dispatches start while they run instead of the GPU idling through every tile's tail.
* **The 2.28 GB of models load once per process and are RESEEDED, never reloaded.** So constructing a
  `TerrainGenV3` is cheap and a tweak rebuild costs nothing unless the seed actually moved. The load is
  DEFERRED until "Terrain/Enabled" — `rebuildMaps` early-outs disabled without constructing the
  generator, **and constructing it is the only load kick.** Disabling the terrain does not unload the models; only
  `unloadModels()` does (below).
* **`isReady()` is a real state.** Until the models are loaded it is false and the generator must not build
  geometry — **it would bake a flat world into the chunk cache.** TerrainStreamer polls it and rebuilds once it flips.

Tiles cache to disk under `Assets/Local/Diffusion/<seed>/` (zstd).

### World origin

`TerrainConfigV3::originX/Z` (the streamer's `Terrain/Origin X (m)` / `Origin Z (m)` tweaks): where
in the seed's model-space world the engine's (0, 0) sits. **Applied at the ONE world → lattice mapping
every sample goes through** (`TileBlock::latticeX/Z`), so point queries and grid fills place the world
identically; the detail and climate noise stay in engine coordinates on purpose (seeded noise, not
model data). The tile caches are keyed in model space and never see it, so moving the origin
regenerates chunks and bakes but no tile already on disk. The lobby's "Seed world" pick sets it
through overrides.

### `TerrainPreview` — the lobby's world overview

`:TerrainPreview`, owned by the App's `Session` (a stack local in main), not a global. `request(seed,
mpp)` constructs a `TerrainGenV3` for the seed (which kicks the model load and RESEEDS the shared
runtime — **so the session disables a live world of another seed first**) and, once ready, runs
ONE Low job that `sampleGrid`s a **256² grid at `ESampleDetail::Coarse`, one texel per coarse pixel**
(`nativePerCoarsePixel() × metersPerPixel` world metres — 33 km across at the clamped 0.5 m/px, 1,970 km
at 30), in 32-row bands so a cancel lands within one band's tile fetches, then colours it (sea by
depth, land by humidity / altitude / temperature, a hillshade, a one-texel coastline; the coarse river network on top,
see [Rivers.md](Rivers.md)) into an immutable RGBA8 `Image` shared by pointer. `worldOffsetAt(u, v)` maps a pick on
the image to the origin offset that puts the spot at (0, 0).

### Generated bounds and the cache-only state

`TerrainConfigV3::bounded` + `boundsMin/Max` (engine metres; the streamer's `setGeneratedBounds`, set
by the session to the seeder's tile-aligned coverage): **inside a bounded generator a FULL tile is
only fetched inside the rect** — `resolveBlock` leaves the slot null past it — **and every sample that
lands on a null slot falls back to the COARSE stage** (`sampleField`, and `sampleGrid` resolves the
coarse block once per grid). So the terrain-data bake's Full near cascade, the collider and the
tree records cost nothing past the playable area, and the streamer's wanted set skips nodes that do not
touch it. Without bounds even a small playable area pulls hundreds of tiles (the chunk ring plus the near cascade).

`TerrainGenV3::unloadModels()` drops the ONNX sessions and enters the **cache-only** state: `isReady()`
(= "sampling is valid") stays true, `modelsLoaded()` (= "inference is possible") is false, a cache
miss is a null tile (coarse fallback, else sea level), and the next `TerrainGenV3` construction
reloads — through the normal not-ready → ready handover. The session unloads at a seeded match's
Start.

**"Terrain/Load models"** (off by default) is the same state as a SWITCH:
`TerrainGenV3::setModelLoadingEnabled`, applied at the top of `rebuildMaps` before any construction.
Off: models already up are unloaded, and `beginLoad` enters cache-only instead of starting the loader,
so the terrain runs entirely on the `.tile` files already under `Local/Diffusion/<seed>/`. `setPrecision`
in cache-only only switches the cache folder (and clears the RAM caches) — it never reloads. The
preview and the seeder gate on `canGenerate()`: the models are up, or loading is off and the caches
serve (a preview then shows only coarse tiles generated before). **A tile missing from disk is retried
from disk on every fetch** — cheap per call, but a camera flying far past the cached area pays one
failed read per tile per grid.

### `TerrainSeeder` — pre-generating the playable area's tiles

Same file (`TerrainPreview.ixx`) and ownership. `start(seed, mpp, origin, halfSize)` constructs a `TerrainGenV3` with
that origin and, once ready, runs ONE Low job over the full-detail tiles covering
`[-half, half]²` in engine space (`fullTileRange`), **nearest the origin first**: each
`prefetchFullTile` is a disk read when the tile is cached under `Local/Diffusion/<seed>/` and ~1.5 s
of inference otherwise, so a previously seeded area comes back in seconds. It only makes the TILES
exist — the mesh, the collider and the bakes stay the streamer's live work — and reports done /
total / cached plus the tile-aligned model-space rect it covers (`coverage`, the preview map's
square). The generator's `fullTileRange` / `fullTileWorldRect` / `isFullTileCached` /
`prefetchFullTile` exist for it. `TerrainStreamer::streamStatus()` remains the "how far has the mesh
ring streamed in" read.

### World scale

**`metersPerPixel` ("Terrain/V3/Meters per pixel") is a UNIFORM scale**: it shrinks elevation and detail by the same
factor as the horizontal, **so a compressed world keeps the model's proportions** — same slopes, shapes and climate,
just smaller and quicker to fly across.

| Value | Meaning |
|---|---|
| **30** | The model's true training scale; continents are continent-sized and peaks ~10 km. A tile is then 7.68 km. |
| **5 (the default)** | A 6× compressed world; a tile is 1.28 km. |
| 3 | A 10× compressed world; a tile is 768 m. |
| 0.3 | A 100× compressed world; a tile is 76.8 m. |

**Lowering it is quadratically more expensive** — the same view distance spans more model pixels, so
more tiles must be generated. `heightScale` is a pure vertical exaggeration ON TOP (1 = real
proportions).

### Sub-pixel detail

The model resolves 30 m/px, so the diffusion field is smooth below that and a close-up hillside would
be bilinear mush.

A **slope-masked** noise layer rides on top (`:Noise` `fbmEroded`), in two bands — coarse crags
(wavelength 220 m, amplitude 38, 4 octaves) and fine break-up (45 m / 11 / 3 octaves) — with
"Detail slope gain" 0.75 as `sf = min(1, slope * gain)`.

> **Plains and seabed stay smooth; only mountain faces get crags.** Wavelengths are in MODEL metres and
> ride `metersPerPixel`, so **the OCTAVE counts are what set how far below the model's 30 m/px the
> terrain actually has anything in it** — that is the dial for "detailed at full world scale".

This is the `sampleAltitude` (macro) vs `sampleHeight` (macro + detail) split.
