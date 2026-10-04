# Procedural trees: generation and rendering plan

Status (2026-10-01): **G1 built** (`.tree` species asset, piece generator, CPU composite) with a temporary
RenderMesh preview (`Trees/Enabled`, see "Trees" in `Code/Procedural/CONTEXT.md`). Everything else is plan.

Confidence tags: `[Certain]` hard evidence, `[Likely]` strong guess, `[Guessing]` filling gaps.

---

## 1. Goals and requirements

* **Target mode: SANDBOX** (fly-through camera, ground level to high altitude). Not the top-down game camera.
* **Enormous counts:** forest out to the terrain view distance (40+ km in each direction).
* **Large shape variety** (species and individuals), but **one shared GPU data layout** for performance.
* **Built for impostors and far tiers from the start.** Every far representation derives from the same data as the
  near tree.
* **Placement consistency:** every tier shows the SAME trees as the scatter. Positions may be off by a few meters.
  **A tree must never disappear when the camera gets closer** (appearing is acceptable, under a crossfade).
* **No "blobs":** far trees keep a species-typical silhouette (conifer tiers, umbrella, columnar, weeping, bare).

## 2. Tier overview

Pixel math (1440p, 90° FOV, ~0.6 mrad/px, 15 m tree) `[Likely]`: ~80 px at 300 m, ~12 px at 2 km, ~3 px at 8 km,
sub-pixel past ~10 km. Forest counts: ~190k trees inside 2 km, ~3M in 2–8 km, ~75M in 8–40 km.

| Tier | Range (initial guess) | Representation | Section |
|---|---|---|---|
| T0 | 0 – ~300–500 m | Procedural geometry: parametric tube branches + leaf clusters | 4 |
| ~~T1~~ | — | **DEFERRED:** octahedral impostor per variant, only if T0→T2 needs a middle step | 5 |
| T2 | ~300–500 m – ~8 km | Terrain-following baked volume clipmap, cloud-style march | 6 |
| T3 | ~8 – 40 km | Analytic canopy layer in the terrain shader | 7 |
| T3s | ridge silhouettes, to the horizon | Cloud-style march over the canopy map, silhouette pixels only | 7.3 |

**T0 hands over directly to T2.** The quality of T2/T3 is judged first; T1 is built only if that hand-over or
the T2 quality near its start is not good enough. Consequences:

* T2's finest level must start where T0 ends — likely a finer level 0 (0.5 m texels, ±256 m) or a longer T0 range.
  The T0↔T2 hand-over distance is the main thing P1/P6 measure.
* T0 may need to reach further than with impostors → T0 cost (leaves, shadows) matters more. `[Likely]`

**Consistency principle:** each tier is the aggregate of the tier below it, computed from the same data
(placement function, piece library + composite function, climate maps). Coverage and color must match at every hand-over.

## 3. Shared foundations

### 3.1 Stateless placement function

Replaces the order-dependent footprint dart-throw in `ScatterSystem` (see `Code/Procedural/CONTEXT.md`) for trees.
One function, implemented identically in C++ (near tier, colliders) and GLSL (far tiers).

1. **Candidates:** one candidate per cell of a jittered grid, one grid per size class (large trees coarse, small
   trees fine). Position, variant, rotation, scale from `hash(cellX, cellY, classId, seed)`. **Integer hashes only.**
2. **Existence by rank:** candidate exists if `u = hash01(...) < density(x)`. `density` comes from the existing
   scatter rules (climate attractors, cluster noise, slope, altitude band above water).
3. **Spacing by priority:** reject a candidate if a higher-priority candidate (larger size class, or higher priority
   hash) exists inside its footprint. Fixed neighborhood, order-independent. Neighbors are tested for existence
   only (depth 1).

**Never-disappear rule** `[Likely]`:

* Far tiers test `u < density_far(x) − ε`, so the far set is a subset of the near set: approach only ADDS trees.
* Far tiers may drop small size classes completely; their coverage goes to the statistical term (T3 / tail).
* Far tiers put trees on the rendered terrain LOD height; a few meters of vertical error is hidden by the crossfade.
* Residual flips through the spacing rule are rare; the tier crossfade hides them. `[Guessing]`

### 3.2 Piece library + GPU tree compositing

Trees are NOT whole pre-generated variants. They are **composited on the GPU per tree** from a library of
pre-generated pieces (section 4):

* **Piece library** per species: trunks (with attach slots) and branch modules (a main branch with its sub-branches
  and leaves). Generated once (CPU, Weber–Penn style per piece; space colonization later if crowns look too
  regular). `[Likely]`
* **Composite function** `composite(tree seed, species, LOD) → list of (piece, transform)`: deterministic, pure
  integer hashing, implemented in GLSL (and in C++ where the CPU needs it: colliders, debug). The SAME function
  feeds T0 drawing, the T2 bake and the aggregate stats — so every tier sees the same tree.
* **Every tree is unique** (different module choice, rotation, scale per slot), memory per tree = its seed.
* **Per-tree variation** on top: rotation about up, scale, lean, leaf tint. The T2 bake applies the same
  transforms. If T1 ever comes back, impostors cannot be baked per unique tree — that would need a separate
  impostor pool (another reason it stays deferred).
* Per piece the library also stores: its crown-cluster ellipsoids (3.3) and aggregate stats (albedo, coverage);
  per species: the expected stats over the composite distribution (for T2/T3 and the canopy map).

### 3.3 Crown-cluster hierarchy

Per **piece**: oriented ellipsoids (leaf clumps) + cones/capsules (branch wood). A tree's clusters = the union of
its pieces' clusters under the composite transforms. Coarse levels per tree: one ellipsoid per module → one per
crown. Each parent's density is set so its average optical depth **matches the children**
(coverage-preserving, like alpha-coverage mips). `[Likely]`

Used as: the source the T2 volume bake splats, the shadow proxy, possibly the T0 far-LOD leaf mass.

### 3.4 Color and species

* Mean color from the existing terrain climate maps: species weight per rule × density × species albedo,
  normalized. Same function for T2, T3, T3s.
* Per-tree species in the near T2 levels (~0.5–3 km), where mixed-forest speckle shows: re-evaluate the placement
  hash of the nearest large-class candidate under the sample, or a point-sampled species-ID volume channel.
  (Open question Q5.)
* Season: global season parameter × per-species color response, all tiers.

### 3.5 World placement: compact tree records per terrain chunk (planned 2026-10-04)

Replaces the camera-placed preview grove. Agreed with the user: **every tree in the terrain ring is stored, as a
compact record per terrain chunk. Render data (mesh / card / billboard pieces) exists only for chunks near the
camera.** Scope: SANDBOX and the lobby "Seed world" (the co-op game uses a flat ground plane, not this terrain).

**What the current code cannot do** `[Certain]`:

* One immutable GPU tree set. A respawn rebuilds it with `graphicsQueueWaitIdle`.
* About 120 B static per tree (64 B cull piece + 48 B volume piece + list) and 64 B of instance stream per drawn tree.
  For the 24 km ring (about 1800 km², about 9M trees at 50/ha) that is about 1 GB. `[Likely]`
* The far volume splats every tree with its 32³ grid at every 64 m rebake.

**The record — 4 B** (6 B if a flags byte for edits is wanted):

| Field | Bits | Note |
|---|---|---|
| x, z | 2 × 12 | Chunk-local, 256 m / 4096 ≈ 6 cm |
| type | 8 | Species (trees and bushes) |

* **No height.** The tree stands on the terrain: near trees take the full-detail sampler height when expanded; the
  far volume takes `terrainHeightAt` from the terrain height cascades. The far cascade covers the whole ring
  (`farRange = max(8192, 2 × meshHalfExtent)`, about 50 km, `TerrainStreamer.cpp`). `[Certain]`
* **No variant / scale / yaw.** They come from `hash(seed, chunk, x, z)` of the QUANTIZED position, identical in
  C++ and GLSL. `[Certain]`
* The far volume uses a fixed density profile per type (the mean of its variants), not per tree.
* **Memory:** about 9M trees × 4 B ≈ **36 MB** (54 MB at 6 B). `[Likely, at 50/ha]`
* **Far chunks store trees only.** Bushes (they fade at 80–120 m) are generated from the placement function when a
  chunk is expanded. Storing them everywhere would add ~4 records per tree. `[Likely]`
* The records are the never-disappear rule of 3.1 for free: near and far read the same trees.

**Generation:** the placement function (3.1: climate attractors per species, cluster noise, slope, altitude band
above water) runs on the terrain chunk's pump job when the chunk enters the terrain ring. One `sampleGrid` per chunk
at `Coarse` detail (~8 m), bilinear reads, slope by finite differences. `.tree` gets numeric climate attractors
(temperature, humidity, spread, density per ha) instead of the bare `Climate` name; the bushes are picked the same
way. A terrain LOD change does not regenerate the records. `[Likely]`

**GPU records:** one large device buffer in pages + a chunk table (page offset, count, chunk origin). Upload
through staging with a per-frame budget; a chunk that leaves the ring frees its pages after `NUM_FRAMES_IN_FLIGHT`
frames. No `waitIdle`. The CPU keeps no copy for far chunks (a chunk can be generated again: pure function).
`[Likely]`

**Near expansion (R_near ≈ 1–1.5 km):**

* **Option A (first):** a job per chunk derives variant / scale / yaw from the hash, takes the exact sampler height,
  adds the bushes, and writes the 64 B cull pieces into a PAGED cull set (add / free per chunk, same per-chunk lists
  from the terrain walk). About 35k trees + bushes ≈ 20 MB. `[Guessing on density]`
* **Option B (later):** no expansion — `tree_cull.inc.glsl` reads the 4 B records and derives the transform in the
  shader. The exact height and the bushes still need a per-chunk step. Measure A first.

**Far volume:**

* **Floor = terrain:** each column's floor is `terrainHeightAt(column centre)`, not the dominant tree's base. This
  removes the two `TREE_FLOOR_PASS` passes and probably the floor smoothing. The march's no-tree fallback already
  uses the terrain height. `[Likely]`
* **Near band (600 m … R_near):** the existing per-tree 32³ splat of the expanded trees, so the blobs match the
  billboards at the hand-over.
* **Far (R_near … 20 km):** a POINT splat — one thread per record adds its type's vertical profile into its one
  column (a tree is smaller than a far column, 40 m+). The dispatch walks the chunk table.
* **Risk — rebake time:** about 9M threads × ~15 atomics per 64 m of camera movement. `[Guessing]` If too slow:
  double-buffer the volume and splat 1/N of the chunks per frame into the back one.
* **Risk — far height resolution:** the far cascade has ~97 m texels (`FOG_TERRAIN_RES` 512 over ~50 km) while the
  volume's columns are 4–42 m past ~2 km: on steep relief a blob base can sit metres to tens of metres off the
  rendered terrain. Too low is mostly hidden (the march stops at the scene depth); floating on a ridge can show.
  `[Likely]` Fixes if it shows: `FOG_TERRAIN_RES` 1024 (~49 m; 4× CPU bake samples, 16 MB per cascade), a middle
  cascade, or sinking the far blobs a little. Test 512 first.

**Benefits of storing every tree:** edits persist (a felled tree is a flag, and the far volume sees it); trunk
colliders in the TerrainCollider ring (96 m) and gameplay / Nav queries read the same records. `[Likely]`

**Out of scope for now:** colliders (Q8), the co-op game map, RT (unchanged: whole-tree billboards within 500 m),
removing the scatter's `tree_small_02` rules (do it when this ships; rocks stay).

## 4. T0 — procedural geometry

### 4.1 Decision: a SEPARATE tree render path, with per-tree bone palettes

Decided with the user (2026-10-01): trees get their **own pipelines and shaders for every stage**, and the
composite data is a **bone palette** per tree (like skinning, but mostly rigid).

Why not the shared mesh path `[Certain]` (`Code/RendererVK/CONTEXT.md`): its vertex layout is fixed
(`MeshVertex` 48 B), the CPU pushes every instance record per frame, and only `MeshInfo` geometry casts into the
cascades. A separate path frees the vertex layout, keeps the per-tree work on the GPU, and costs:

* an own **shadow-cascade draw** (recorded into the cascade pass),
* an own hook into the opaque scene stage (`buildSceneStages`) and its motion-vector target,
* **no RT / GI at first.** RT must stay ADDABLE: keep the pieces' positions extractable as plain triangles
  (a per-piece BLAS from the tree vertex buffer), so trees can enter the TLAS later as per-piece instances
  with palette transforms (rigid bones) or as proxies. Do not design anything that rules this out.

Design `[Likely]`:

* **Piece library** in one tree-owned vertex/index buffer, compact layout: position, octahedral normal, UV,
  1–2 bone indices + weights (piece-local).
* **Per-tree bone palette** = trunk bone + one bone per filled slot + the modules' internal bones (G1 already
  generates these: bone 0 per piece, one per level-1 sub-branch). Written by a compute pass when a tree becomes
  resident (the GLSL composite function), persistent.
* **Wind** = a per-frame compute pass over the BONES (tens per tree), not the vertices. Last frame's palette is
  kept for motion vectors.
* Per frame: **tree cull** (per view: main + each cascade) → **expansion** (visible tree × slot → piece instances
  bucketed per piece mesh/LOD) → `drawIndexedIndirectCount` with a skinning vertex shader; the fragment shader
  reuses the engine's lit-shading includes (sun, shadows, GI probes, fog) so trees light like everything else.
* Shadows: the same vertex shader, depth only.

The G1 preview draws through the plain RenderMesh path (one node per placed piece) until G4 replaces it.

### 4.2 Piece library (generated once, CPU)

* Lives in `Procedural` (holds scatter, links RendererVK). Deterministic from `(species params, piece seed)`.
* **Trunks** (per species, e.g. 4–8): trunk + the first branch stubs, with **attach slots**: height `u`, azimuth,
  local radius, outward direction, max module length (from the species' crown envelope at that height).
* **Branch modules** (per species, e.g. 16–32, in 2–3 size classes): a main branch with its sub-branches and
  leaves. Base frame at the origin, base radius, length, bounds, crown-cluster ellipsoids.
* Optional later: **twig/leaf clusters** as a third level, if modules repeat visibly.
* Generation per piece: Weber–Penn style skeleton → swept-tube bark (UV = `(u, θ)`, `u` scaled by length for a
  constant texel density) → leaf cards from a leaf atlas, alpha-tested (`LitMasked`).
* **Piece LOD chain (own, NOT meshopt):** fewer rings/sides, branch-order cutoff, fewer + larger leaf cards with the
  same total coverage, a per-level error for the screen-space-error selector. meshopt thins foliage. `[Likely]`
* Two meshes per piece: bark and leaves (alpha-tested later), shared materials per species.
* Memory: the library is small (a few hundred pieces); per tree only a seed (+ its palette while resident).
* **Built in G1** (CPU, `TreeGenerator.cpp`): trunks with slots from the crown envelope, modules with up to 3
  sub-branch levels, double-sided diamond leaf cards (placeholder, no texture), per-vertex bones.
  Not yet: size classes, piece LODs, leaf atlas.

### 4.3 Composite function (GPU, deterministic)

Per tree, from `hash(tree seed, slot)`:

1. Pick a trunk; apply tree scale / lean / rotation about up.
2. For each trunk slot (a species-dependent fill fraction): pick a module of the matching size class, rotate it
   about its own axis, **scale it to fit the slot** (length to the crown envelope at that height, base radius
   ≈ slot radius), tilt jitter. **The species' crown envelope (cone, sphere, umbrella, column) sets the silhouette;
   the modules fill it.**
3. Module base sinks slightly into the trunk to hide the joint.

Variety comes from: trunk choice × module choice per slot × rotation × scale × fill. Pieces are rigid, so they
cannot bend to fit — only scale and tilt. `[Likely]`

**Tree LOD = fewer pieces:** at distance drop the small-class modules (their coverage moves to larger modules /
a scale-up), then use a few "crown chunk" pieces. The piece LOD chains do the rest.

### 4.4 The tree pipeline (G4, the main new work)

A `TreePipeline` in RendererVK, structured like `ParticlePipeline` (compute passes + own draws), fed by
`Globals::trees`. Stages `[Likely — details to settle in G4]`:

1. **Residency:** placed trees (position, seed, species) → palette slots; the composite compute writes the
   palette once.
2. **Wind** (compute, per frame): animate the palette bones; keep the previous palette.
3. **Cull + expansion** (compute, per view): visible tree × slot → piece instances per piece mesh/LOD →
   indirect draw args.
4. **Main draw** in the opaque scene stage (with motion vectors), **shadow draw** into the cascade pass.

Points to settle:
* Capacity: ~50k visible trees × ~20 pieces = ~1M piece instances before culling; per-cascade expansion cost.
* Leaf tint per tree, season, species materials.
* The RT hook-in point (4.1) — keep it possible, do not build it.

### 4.5 Scatter integration

* A `ScatterAsset` can name a tree species instead of an `.oc`. Placements become (position, seed) records on the
  GPU, not `RenderNode`s. Later the stateless placement (3.1) generates them on the GPU directly.

### 4.6 Wind — on the bones

**Branch sway = a per-frame rotation of each bone about its pivot** (noise by tree seed + bone), in the wind
compute pass. The main draw and the shadow draw both skin with the same palette, so shadows sway too. Trunk sway
on bone 0. Only leaf flutter needs a vertex-shader term (later). `[Likely]`

### 4.7 Known risks

* **Visible repetition** of modules: mitigate with more modules per class, rotation, scale, fill fraction, and a
  twig/leaf-cluster level later. `[Likely]`
* **Joints** between rigid pieces: base sink + radius match by scale; non-uniform scale distorts the bark texel
  density, keep it small.
* **No depth prepass** (forward renderer): dense alpha-tested leaves shade every overdrawn layer. Leaf overdraw is
  the expected main cost. Mitigations: tight cards, fewer larger cards in LODs, a leaf-only depth prepass later.
  `[Likely]`
* Instance count / cull cost of the expansion (4.4).
* Textures: bark + leaf atlas assets are needed (Q10).

## 5. T1 — octahedral impostors (DEFERRED)

**Not in the plan for now.** Build only if T0→T2 shows one of these: visible pops at the hand-over, T2 too soft
near its start, or T0 too expensive at the distance T2 needs. Kept here as the design if it comes back:

* Baked per variant right after generation, **from the last T0 LOD** (same leaf density and silhouette at the switch).
* Hemi-octahedral, e.g. 8×8 frames of 64 px = 512² per variant, texture array.
  Channels: albedo + alpha, normal + depth, AO, thickness/transmission, leaf/bark mask.
  ~0.5 MB per variant (BC7/BC5) → ~128 MB for 256 variants. `[Likely]`
* Rotation about up = rotate the view vector before the lookup. Tint and season through the leaf mask.
* Writes `gl_FragDepth` (terrain intersection, stable crossfade, sun-view lookup for shadow cascades).
* **Coverage-preserving alpha mips** — otherwise far foliage thins out.
* Lit with the same inputs as T0 (GI probes, sky SH, shadows, transmission) — tier pops are mostly brightness.
* Dithered crossfade T0↔T1, hidden by TAA/DLSS.

## 6. T2 — terrain-following baked volume (hybrid cloud march)

### 6.1 Data

* **Clipmap volume around the camera:** xz in world space, y = height above the terrain (`y − h(x,z)`),
  ~40–60 m tall. Vertical offset (not rotation) is physically correct: trees grow vertically.
* Levels (texel ≈ pixel footprint) `[Likely]`:

  | Level | Texel | Range (1024² × 32 slices) |
  |---|---|---|
  | 0 | 1 m | ±512 m |
  | 1 | 2 m | ±1 km |
  | 2 | 4 m | ±2 km |
  | 3 | 8 m | ±4 km |
  | 4 | 16 m | ±8 km |

  R8 per channel = 32 MB per level per channel; BC4 halves it; fewer slices on the coarse levels.
* **Channels:**
  * R: signed distance to the clump surface (union of the cluster ellipsoids) — sharp edges at any texel size:
    `density = saturate(−sdf / pixelFootprint)`.
  * G: coverage/density — the optical depth; preserves coverage in coarse mips where the SDF loses thin features.
  * Baked lighting: sun transmittance, ambient occlusion (refresh slowly as the sun moves).
  * Optional: point-sampled species ID (Q5).
* 2D per level: **max canopy height** mip (space skipping).

### 6.2 Bake

* Compute pass splats the crown-cluster hierarchy (cut by level texel size) of every tree from the placement
  function into the new clipmap strips. Toroidal update, like the terrain/ocean clipmaps.
* **Bake and march must use exactly the same `h(x,z)` per level** — otherwise crowns float or sink. `[Certain]`

### 6.3 March

Reuses the cloud march structure: slab clip → max-height skip → steps (one 3D + one 2D height fetch) → early exit
on transmittance → aerial perspective in front of the hit. Cloud techniques reused:

* erosion noise at sub-texel scale, faded below the pixel footprint,
* per-species vertical profile,
* leaf transmission / back-light via the cheap cloud light terms,
* half-res + temporal reprojection (**risk: thin, high-contrast silhouettes against the sky ghost**),
* outputs depth at the transmittance-0.5 point (composite, fog, upsample edges).

**Analytic tail:** after the clipmap's range (or after K steps on grazing rays), finish with the T3 slab formula.

## 7. T3 — analytic canopy layer

### 7.1 Data

Per terrain texel, derived from the same density field (NO instances): crown cover `c`, mean height `h`,
mean albedo/roughness (section 3.4).

### 7.2 Shading (Beer–Lambert slab / gap fraction)

```
gap(θ)  = exp(-k / cos θ),   k = -ln(1 - c)
cover   = 1 - gap(θ_view)
color   = cover · crownLit(θv, θs) + (1 - cover) · groundLit · gap(θs)
```

* View-dependent cover (forest closes at grazing angles), ground shadowing by the sun angle, crown self-shadow,
  **hotspot** (correlate view/sun gaps near the anti-sun direction).
* Terrain vertices displaced by ~`h · c`: depth, fog distance and shadows at crown-top height.

### 7.3 T3s — ridge silhouettes

A flat layer shows no tree line on ridges against the sky. Cloud-style march with the canopy map as the "weather
map" and per-species vertical profiles, **only on pixels near the terrain silhouette**. `[Guessing]`

### 7.4 Hand-over

`cover_T3 = cover_total − cover_T2` across the fade band; both from the same `c` and `gap(θ)`.

## 8. Shadows and ray tracing

* T0: normal cascades; static tree shadows cached per cascade / page where possible.
* (T1, if built: impostor sun-view lookup in the cascades.)
* T2/T3: canopy shadow map (top-down density/height projected along the sun — same pattern as the cloud shadow
  map); the T2 volume carries its own baked sun transmittance.
* Ray tracing: one BLAS per piece chain; limit TLAS piece instances by distance (4.4);
  `VK_EXT_opacity_micromap` for alpha-tested leaves.

## 9. Phases

Each phase ends with something the user can look at in SANDBOX and measure with `Tools/profile.ps1`
(RelWithDebInfo, no `-Game`).

**Order: the tree generator and T0 first**, then the far tiers on top of the same composite function.

| # | Phase | Exit criterion |
|---|---|---|
| G1 ✅ | **Piece generator:** `.tree` asset, Weber–Penn style skeleton, swept-tube bark, `(u, θ)` UVs, placeholder leaf cards; trunks with attach slots, branch modules, per-vertex bones; piece-library view in SANDBOX | Pieces read as the species' parts — **user to judge** |
| G2 ✅ | **CPU composite (prototype):** the composite function in C++, trees spawned as plain `RenderNode`s per piece, a small grove | Composited trees look like whole, varied trees at 5–100 m; joints acceptable — **user to judge** |
| G3 | **Live editing:** species params as tweaks / a panel, regenerate pieces + re-composite on change | Species can be tuned without restarting |
| G4 ✅ (step 1) | **GPU expansion into the EXISTING instance stream** (decided 2026-10-02 instead of a separate pipeline). First as a `tree_expand.cs.glsl` pass writing every piece's records into the host-visible stream each frame (~7.5 ms GPU); the same day replaced by **BAKED TREE RECORDS**: static device-local pieces/types, the culls build the records in a claimed stream range (`tree_cull.inc.glsl`) — no per-frame pass. The CPU does one claim + per-mesh counts per frame. The TLAS writer builds the trees' RT instances (the billboard) from the same static data, the material carried in the custom index (bit 23) - GI / RT see the trees again without any per-frame tree data. Still placed by the CPU composite (pieces uploaded once); not yet: composite in GLSL, per-tree records, bone palettes / wind, a coarse per-tree cull | Same trees as the CPU path; the "Trees" CPU scope near zero — **user to judge** |
| G4b ✅ | **Baked whole-tree variants** (2026-10-02, user: per-piece instancing far too expensive — a 64² grove was ~200k instances): `Bake Variants` (4) merged trees per species per LOD level, placed with seeded variant/scale/yaw → 3 records per TREE; whole-tree billboards / impostors from the same machinery. Next: a per-tree VS **warp** (bend, twist, lopsided crown, noise; seeded by position) for near-range uniqueness + wind on the same terms, with a shadow VS variant; device-local expansion records; scaling past ~100k trees (per-tree GPU cull + compaction, or cell nodes) | ~16x fewer records; grove looks right — **user to judge** |
| G5 ✅ (preview) | **LODs:** 4 piece mesh LODs from one skeleton as GPU LOD chains; **branch-module billboards** (two crossed cards: side + top view, CPU bake, PNG cache, cast shadows; default) with **octahedral impostors** kept as the fallback far mode (`TreeImpostor` pipeline variant), per-module distance switch — replaces whole-tree T1 (composited trees cannot share a whole-tree impostor) | No visible pops 0–500 m; instance / triangle counts measured — **user to judge** |
| G6 | **Scatter integration:** `ScatterAsset` → species, placements as GPU records; measure 40–100k trees — **superseded by W1–W5 (3.5)** | Forest to ~500 m within budget (`Tools/profile.ps1`) |
| G7 | Wind: rigid module sway in the expansion pass | |
| G8 | Side outputs: per-piece cluster ellipsoids, per-species stats, trunk capsules | Data ready for the far tiers / physics |
| P1 🚧 | (2026-10-02, built, user-untested — see RendererVK "Far-tree volume") **Changed with the user:** no G8 clusters and no hash-grid placement — the GPU tree SET's trees are splatted (per-variant 32³ extinction grids) into ONE camera-centred POLAR volume (angle × log radius from "Far start" 5 km to "Far end" 40 km — beyond the billboards; cells grow with the distance), z = height above terrain; re-baked on a camera snap. Original: **T2 march prototype** with the G8 cluster data (bake = the GLSL composite function): fullscreen compute, hash-grid placement in GLSL, ONE fixed-size volume (no clipmap), depth composite vs terrain | Cost at grazing angles acceptable; cluster trees read as trees at 1–5 km. **Go/no-go for T2.** |
| P2 | T2 clipmap levels + toroidal bake + max-height skipping + analytic tail | Stable while flying; cost budget held |
| P3 | Stateless placement in C++ and GLSL; switch the tree scatter rules over; verify subset rule | Far set ⊆ near set (debug view: far-only trees highlighted) |
| P6 | T0↔T2 crossfade; tune the hand-over distance and T2's finest level | No visible pops at the hand-over. **Decides if T1 is needed.** |
| P7 | T3 analytic canopy layer + T2↔T3 hand-over | Forest edges stable from 100 m to 40 km |
| P8 | Shadows across tiers, baked volume lighting, canopy shadow map | |
| P9 | T3s silhouettes, per-tree species in T2, seasons, ray tracing | |
| (P10) | Whole-tree T1 impostors — superseded by the per-module impostors of G5 | |
| W1 🚧 | (2026-10-04, built, user-untested — Procedural CONTEXT "World records") **World records (3.5):** record format, hash-derived attributes (C++ + GLSL), numeric climate attractors in `.tree`, placement on the terrain pump job | Debug: tree count per chunk; species follow the climate — **user to judge** |
| W2 🚧 | (2026-10-04, built, user-untested — RendererVK "WORLD TREE RECORDS"; a fixed pool of 64-record blocks, no growth) GPU record pages + chunk table (staged upload, deferred free, no `waitIdle`) | Flying in SANDBOX: no stalls; memory as estimated |
| W3 🚧 | (2026-10-04, built, user-untested — Procedural CONTEXT "World mode"; RendererVK dynamic tree sets) Near expansion, Option A: paged cull set, R_near hysteresis, bushes from the placement function; the preview grove becomes a debug option | Forest around the camera in the whole world — **user to judge** |
| W4 🚧 | (2026-10-04, built, user-untested — RendererVK "WORLD TREE RECORDS"; every tree from the records: within "Far record detail" expanded exactly on the GPU and splatted in detail, beyond as a 2D mass per column + the type's height profile; the world set never splatted; the rebake not yet measured or amortized) Far volume from the records: terrain floor, near-band detailed splat + far point splat; measure the rebake, amortize if needed | Far forest matches the climate; rebake within budget (`Tools/profile.ps1`) |
| W5 | Profile + tune R_near, density, `FOG_TERRAIN_RES`; later Option B, colliders, edits | |

(P4/P5 from the first draft are now G1–G7.)

## 10. Open questions

* **Q1 — GPU budget** for all tree tiers at 1440p (initial guess: ~1 ms for the T2 march). Which reference GPU(s)?
* **Q2 — T0→T2 hand-over distance** (T1 deferred): how near can T2 start before it looks too soft, and how far
  can T0 reach within budget? Decided by P1/P6; also decides if T1 is needed.
* **Q3 — Volume memory:** levels, slices, BC4 vs R8; is ~80–160 MB acceptable?
* **Q4 — Placement change scope:** do rocks/grass keep the dart-throw, or does all scatter move to the stateless
  function?
* **Q5 — Per-tree species in T2:** placement-hash lookup vs species-ID channel.
* **Q6 — Generator:** Weber–Penn style only, or also space colonization? Authoring format for species params
  (new asset type, or tables like `scatterRules()`)?
* **Q7 — Species count** at once (see also Q13 for pieces per species).
* **Q8 — Colliders:** physics for near trees (trunk capsules from the composite function, C++ side)?
* **Q9 — Wind at distance:** none in T2/T3, or a slow density wobble?
* **Q10 — Bark and leaf textures:** existing assets, new downloaded/authored ones, or procedural (generated
  leaf atlas)? Needed by G1/G2.
* **Q11 — Piece meshes with LOD chains:** extend `RenderMesh` with an authored LOD chain + per-level error, or
  build an in-memory `ObjectContainer` with `LodN_` nodes (authored chains carry no error data → falls back to
  projected diameter)?
* **Q12 — GPU instance path design** (4.4): append into the existing `InstanceStream` vs a separate tree stream
  merged by the cull; persistent per-tree transforms vs per-frame expansion; per-tree tint through the instance.
* **Q13 — Library size:** trunks / modules per species, size classes, and whether a twig/leaf-cluster third
  level is needed against repetition.
