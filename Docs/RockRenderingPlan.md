# Procedural rocks: generation and rendering plan

Status (2026-10-05): **R1 built** (the `.rock` loader, the SDF generator, the preview field - "Rocks" tweaks, see
Procedural CONTEXT "Rocks"); the shapes had a first round with the user (erosion + warp, superellipsoid blocks,
piles, the columnar outcrop - the last two types then REPLACED from the user's reference photos, 2026-10-06: 3).
**R2 built, user-untested** (the `LitRock` material: 5.1). **R3 built,
user-untested** - and CHANGED: no tessellation, a regular mesh LOD chain (2). **R4 built, user-untested** (rocks in
the world: 6). **R5 built 2026-10-06, user-untested** (the far volume: 7). R6 not started. Companion to
`Docs/TreeRenderingPlan.md`: rocks use the tree machinery again where they can, and this document names each
place where they differ.

## 1. Goals

* Boulders and rocks in the generated world, placed per terrain chunk like the trees.
* Two tiers: a regular mesh LOD chain (the GPU picks each rock's level), and the FAR VOLUME (the far-tree volume,
  shared). See 2.
* The rock TYPE sets the SHAPE only (plus a small number of baked variants). The CLIMATE at the rock sets
  the colour and the texture.
* A rock must look like the cliffs around it. The terrain already picks a bedrock texture per climate
  (`TERRAIN_TEX_SOURCES`, kind `Rock`: gray granite, mossy pitted, weathered stone, sandstone, red rock,
  basalt). **Rocks use the SAME six bedrock materials, picked by the same `pickClimate`.** Then a boulder
  under a sandstone cliff is sandstone, with no per-type colour authoring.
* **Rocks can be BIG**: from ~1 m up to 22 m. The upper limit is the far volume's layer height (`Far height`,
  22 m) - a HARD limit (the user, 2026-10-06; `ROCK_MAX_SIZE` clamps every type's `Scale`). Everything about
  placement, tiers and detail must scale over that range.
* **No SMALL rocks here** (the user's decision, 2026-10-05): pebbles, scree and other small ground objects
  (fallen branches too) belong to a later, separate ground-clutter scatter system.
* **The record stays the trees' 4 bytes** (x, z 12 bits each, type 8 bits). No per-instance surface data: the
  climate comes from the terrain-data map at the rock's position (5.2).
* Replace the current scatter rules for `boulder` / `rocks` (the namaqualand `.oc` models in
  `Scattering.cpp`) when the new path works.

Out of scope: physics colliders and Nav obstacles (see 10, open questions), destruction, rock arches and
overhangs at terrain scale (that is terrain work).

## 2. Tier overview

**Decision (2026-10-05, the user's, replacing the same day's "every type is tessellated from its SDF"): NO
tessellation - a REGULAR MESH LOD CHAIN.** The tessellated route was not worth its cost: a new pipeline route
through the instance set, a GLSL copy of the SDF plus a residual 3D texture per variant, and fixes for fracture edges
and concave features - for detail the bedrock normal map already gives per pixel.

| Tier | Range | Representation | Shadow | RT (GI / RT shadow / RTAO) |
|---|---|---|---|---|
| R0 | 0 .. `Far start` (500 m; later for big types, 7.5) | the mesh LOD chain: up to 4 levels, level 0 at the type's `Lod` triangles (~2000), each next a quarter | the chain, coarser (the shadow cull's own pick) | the chain's ONE BLAS |
| R1 | `Far start` .. `Far end` | far volume (shared with trees) | the volume's own | none |

* **The GPU picks the level PER INSTANCE**, each frame, from the projected simplification error (RendererVK "Mesh
  LODs": `Max error (px)`, hysteresis) - the engine's standard chain (`Renderer::createMeshLodChain`). A switch
  happens where the two levels differ by less than the pixel threshold, so it needs no crossfade band.
* **It needs NO per-instance data, so the 4-byte record is enough** [Certain]: the level is a function of the
  rock's distance and scale, computed in the cull. (The main cull's hysteresis slot is GPU-side state per drawn
  instance, not record data; a stateless pick also exists - the shadow cull's.)
* **The instance-set route (R4) needs one renderer change**: the culls skip the LOD lookup for tree-set records
  today (tree meshes have no chains - `cullInstance`'s `isTree`). For rock records it comes back, stateless
  (`lodSelectLevel` with no last level), and the TLAS writer's `treeCullRtPiece` must fall back to a type's
  `bark` record (a rock type has no billboard and no leaves).
* Shapes with thin features need the cells and the triangles: a type asks for them itself (`Resolution x`, a
  multiplier on `Rocks/Grid resolution`, and its `Lod` count) - the `Monolith`'s strata take `Resolution 2.5`.

## 3. The rock type asset (`.rock`, `Assets/Rocks/*.rock`)

One file per type, loaded like `.tree` (Procedural, NOT AssetRegistry - same reason as trees). **The built
grammar is in Procedural CONTEXT "Rocks"** (`loadRockType`, RockType.cpp). Planned, not built: `Tilt` (a random
lean) and `Cluster count radius sizeRatio` (satellite rocks at expansion, R4).

Placeholder types (shape families, not climates; `Assets/Rocks/`):

| Type | Shape | Typical where |
|---|---|---|
| `Boulder` | superellipsoid near an ellipsoid, strong erosion, few shallow fractures, noise, pits | glacial / granite fields, any climate |
| `Block` | superellipsoid (a rounded block), 6 fractures, ridged noise | rockfall: cliff bottoms, valley floors - flat-ish ground only |
| `Slab` | flat superellipsoid (`Squareness 4`), strata | sandstone / slate on flat-ish ground; common in the savanna |
| `Pinnacle` | `Shape Pillar`: tapering spires, alone or up to 3 merged at the base (`Group`), vertical flutes, 1-4.5 m, upright | a hot desert, flat ground, dense fields |
| `Monolith` | `Shape Pillar`: one weathering pillar - a narrow foot, widest above the middle (`Profile`), deep strata, a blocky section, 10-22 m, upright | flat-ish ground away from rugged ground and valley floors, any climate; a few together, then none for a long way |

`Pinnacle` and `Monolith` (2026-10-06, user-untested) replaced `Column` (a columnar-basalt outcrop of packed
hexagonal prisms) and `Tor` (a pile of 3 blocks): the user gave a reference photo for each (limestone pinnacles in
desert sand; a layered weathering pillar on a plateau, "flat-ish ground, fully upright"). Both are ONE new
primitive, a standing body whose width follows a 5-point profile over its height, on a flat floor.

**Triangle budgets**: level 0 = the type's `Lod` (2000 triangles, ~1000 vertices, for the simple types; 1500 for a
`Pinnacle`, 6000 for the 22 m `Monolith`). The user's earlier budget ("about 500 vertices, 1k at most") was for a mesh BEFORE
tessellation; with regular LODs level 0 carries the silhouette itself, and ~1000 vertices stays inside it.

The climate box on a type is OPTIONAL and coarse: it can say "pinnacles only in the desert", but the colour
never comes from the type.

## 4. Shape generation (CPU, once per type at load)

### 4.1 SDF, then the LOD chain (as built; Procedural CONTEXT "Rocks")

1. **SDF** per variant (seed = type seed + variant): the `Shape` body (superellipsoid blocks, a `Pile` of them, or
   a `Group` of standing pillars with a width `Profile`) with `Aspect`, `Fracture` as intersections with random half-spaces, `Erosion` (a Minkowski
   rounding), `Warp`, `Strata`, `Noise` / `Ridged`, `Pits`, `Split`. CPU only: nothing evaluates it on the GPU.
2. **The full mesh**: surface nets on a `Rocks/Grid resolution` grid (32; 96 for a finer source), every vertex
   projected onto the field and shaded from its gradient. CPU working data, never uploaded.
3. **Cavity** per vertex of the full mesh: five field taps out along the normal (Quilez' SDF occlusion), 1 = open,
   0 = deep in a crevice. It rides the mesh's unused u channel to the rock shader: AO, and where moss / dust gathers.
4. **The LOD chain**: `meshopt_simplify` of the full mesh to the type's `Lod` triangles (level 0), then a quarter
   per level (up to 4 levels, at least 24 triangles), each from the FULL mesh, with its error in rock-local units
   for the GPU's screen-space-error pick.

Not built: dual contouring and locked fracture edges for sharper cuts (surface nets round an edge at cell size).

### 4.2 Cache

As the tree textures: a mesh cache under `Assets/Local/Rocks/` keyed by a hash of the type's parameters and a
`ROCK_GEN_VERSION`. Generation at 96³ for ~6 variants x 5 types is cheap [Guessing: well under a second on the
job system], so the cache is optional; skip it in R1.

### 4.3 Far-volume density per variant

Voxelize the SDF directly into the 32³ extinction grid (`TREE_DENSITY_RES`): inside = a high extinction
(solid), with a one-voxel soft edge. No surface triangles needed. The per-variant mass and the height profile
follow from it, as `bakeTreeDensity` gives them for trees.

## 5. Surface: colour from the climate

### 5.1 The rock shader path

**BUILT (R2, 2026-10-05, user-untested): the `LitRock` pipeline variant** (`instanced_indirect_rock.vs/.fs.glsl`,
`RockParams` / "Rocks/Material" tweaks) - its own variant, as the terrain's, so the rock code does not set the
register count of every lit mesh. **As built: RendererVK CONTEXT "The rock material".** Per pixel:

* **Bedrock material**: the climate pick `pickClimate(climate, numGround, numRock)` from `terrain_splat.inc.glsl`
  (up to three entries, blended) - the same entries the cliff behind the rock shows.
* **Biplanar in WORLD space** (`sampleTerrainTriplanar`): rocks are static, so world space does not swim, and every
  instance of a variant gets different texture placement for free. The terrain's rock UV scale x "UV scale"
  (default 4: at the cliffs' own 20 m tile a 2 m boulder shows a tenth of the texture).
* **Cover layers on up-facing surfaces**, from the same terrain rules: SNOW (the terrain's rule with the rock
  normal's Y in place of the terrain slope - snow sits on the top and slides off the sides), and GROUND cover (the
  climate's ground pick: moss in the rainforest box, sand / dust in the deserts) by up-facing x a patch noise,
  and in the crevices by the baked per-vertex CAVITY ("Cavity cover"); the cavity is also the rock's AO ("Cavity
  AO").
* **Ground contact band**: over the lowest "Contact height" (0.4 m) above the terrain, blend toward the ground
  material and darken (AO). This hides the cut line where the rock enters the ground.
* Wetness: the terrain wetness clipmap darkens and glosses the rock as the ground (binding 18, set-wide).

### 5.2 Per-instance data

NONE. The record and `TreeInstancePiece` stay as they are.

* **Climate**: [Certain] the terrain-data map's B channel packs the sea-level temperature baseline and the
  humidity (bits 16-23 / 24-31; `terrainClimateAt` / `terrainClimateNearestAt` in `terrain_height.inc.glsl`,
  the ocean FS already reads it). The rock VS reads `terrainClimateAt` at each VERTEX and evaluates
  `terrainTemperatureAt(climate, vertex Y)` there (linear in height: the interpolation is exact) - the lapse rate
  then makes the top of a 20 m boulder colder than its foot, as on the terrain.
* **Contact band**: the ground height from `terrainDataAt` at each vertex's xz. The near cascade's texels are
  metres wide (the band follows the slope, not the fine relief: it is a soft gradient, not a line); the far
  cascade is too coarse, so the band fades out by "Contact fade distance" (200 m).
* The shader runs `pickClimate` from those values, so the material choice follows any edit of
  `TERRAIN_TEX_SOURCES` with no rock rebuild.

### 5.3 Why not bake colours per variant

A baked albedo per variant (the trees' route) ties the colour to the type, and the user wants the opposite. It
also cannot follow a climate gradient across one boulder field. The triplanar terrain material costs more
per pixel, but rock pixels are few compared with terrain pixels. [Guessing]

## 6. World placement: rock records

**BUILT (R4, 2026-10-05, user-untested). As built: Procedural CONTEXT "World records" (ROCK RECORDS) and "World mode"
(THE WORLD'S ROCKS), RendererVK CONTEXT "BAKED TREE RECORDS".** Where the build differs from the plan below:

* One WORLD lattice per rock TYPE (cell = 1.5 x its largest size), not per size class.
* **No rock-against-rock rejection**: boulders lie against boulders, and a rejection order would chain across chunk
  borders. The trees give way to every rock, the neighbours' included (a 3-point grid halo, +24 % samples per chunk).
* Talus = gentle ground with a steep slope 6 m uphill (one extra field sample), not a 30 m search.
* The crag fit mirrors the terrain's rock coverage at its DEFAULT texture tweaks (constants, not the live tweaks).
* Satellites (`Cluster count radius sizeRatio`) and `Tilt` are not built.
* Rocks draw as meshes wherever their chunk is in the set (out to the near radius): the far-volume hand-over is R5.
* **PLACEMENT IS PER TYPE, BY RULES THAT ADD** (2026-10-05, the user's decisions; user-untested). A type has any
  number of `Placement` blocks and their densities add: a rule for everywhere, plus a rule per climate the type is
  common in (`Slab.rock`: common in the savanna - the Acacia's climate box - on the plains too). The GROUND keys
  of a rule, density multipliers: `Plains x` (flat ground), `Rugged low high` (rugged ground - the steepest 8 m
  grid cell within 2 cells of the rock over a slope of 0.08..0.4 - at the LOW and at the HIGH end of the heights
  within ~20 m: a cliff's bottom / its top) and `Valley x` (low ground with higher ground within ~160 m, from a
  coarse 32 m grid: valley floors, mountain feet - there the multiplier IS x). The slope under the rock cannot tell
  a cliff's foot or a valley floor from a plain; it has its own key, `Slope`, with soft edges (a boulder rolls off a
  slope). The user's requests behind them, in order: large types off the plains, to the cliffs and hills; way more
  rocks in valleys and at cliff bottoms ("rugged weighted to the lower end of the height"); boulders not on slopes.
  `Plains 0` = never on plains (Block). A first version scaled LARGE rocks down on plains for every type:
  removed - the user wants the control per type, not a smaller average size. Chunk-generation cost is not a
  concern to the user (the per-frame cost is). Procedural CONTEXT "ROCK RECORDS" and the `.rock` grammar.
* **A rock FOLLOWS THE GROUND NORMAL** (2026-10-05, the user's decision: "not be upright all the time"). `Align` (a
  key of the TYPE now, not of a Placement block)
  defaults to 1 (was 0.3: the rocks read as upright). One rule for the world and the preview,
  `RockSystem::groundTransform`: the turn from up to the footprint's normal x `Align`, the origin on the footprint's
  plane (lowered for a dip or a saddle), sunk along the ROCK's up axis by `Sink` x height plus what the part of the
  slope it does not follow lifts its downhill edge (r x tan of the angle left). That replaces "Sink = max(Sink x
  height, the height range under the footprint)" below, which buried the uphill side of an upright rock.

Rocks join the TREE RECORDS (`TreeWorld`, Docs/TreeRenderingPlan.md 3.5), not a second system:

* [Certain] `TreeRecord` already has 8 type bits (256 types) and no species-specific data. Rock types append to
  the name-sorted list (or the list becomes trees, then rocks). The 4-byte record, the ring, the pump jobs, the
  GPU pool, the CPU keep radius, the generation counter - all unchanged.
* `placeChunk` runs a SECOND candidate lattice for rocks (its own `Candidate cell`, ~4 m), so rocks do not
  compete with trees for the one-per-cell slot. A rock candidate is rejected inside a tree's trunk radius.
* Rock fit = the climate fit (when the type has a box) x the slope band x
  `mix(1, cragW, Crag weight)` x `mix(1, talusW, Talus weight)` x the cluster fbm, where:
  * `cragW` = a CPU mirror of the terrain shader's rock coverage (slope + crag smoothsteps, no wander fbm):
    rocks lie where bedrock shows;
  * `talusW` = a flat-ish cell with a steep cell uphill within ~30 m (from the same `sampleGrid`): scree and
    fallen boulders at the foot of cliffs.
* **Big rocks need a FOOTPRINT, not a cell.** One rock per ~4 m cell cannot hold a 20 m boulder. Each rock type
  has a size class, and each class its own candidate lattice with a cell about its footprint (e.g. 4 / 12 / 32
  m). Classes place largest first; a smaller candidate inside a placed rock's footprint is rejected (the
  scatter's dart-throw order). The record still stores only x, z, type - the scale comes from the seed.
* **Rocks before trees, across chunk borders.** A tree inside a boulder must not exist, also when the boulder
  belongs to the NEIGHBOUR chunk. So the rock lattice must be a pure function of the CELL (world seed + cell
  coordinates + the sampler), not of the chunk: `placeChunk` evaluates the rock cells of a halo of the largest
  footprint radius around its chunk, keeps only its own rocks as records, and uses the halo rocks for the tree
  rejection. Same rule for big rocks overlapping each other across the border.
* **Sink and align scale with size**: a 20 m boulder on a 30 % slope needs ~3 m of sink on the downhill side.
  Sink = max(the type's `Sink` x height, the ground's height range under the footprint), from the
  `sampleGrid` the expansion already takes.
* **Satellites** (`Cluster count radius sizeRatio`) are NOT stored. Expansion makes them from the record's seed,
  as it makes a tree's bushes: a few smaller rocks of the same type beside a big one, never below ~1 m (small
  debris is the later ground-clutter system's). They enter the far volume only as the parent's extra mass (the
  far splat does not expand them).
* One dynamic instance set draws per frame [Certain: "ONE set draws per frame", Renderer.ixx], so the rock types are
  extra `TreeInstanceType`s of the world's set (its `bark` record = the chain's level 0 on `LitRock`; no billboard),
  and `expandChunk` emits rock pieces next to tree pieces. Raise `Set capacity (pieces)` for the rock count
  (measure first: `Log stats` per type). The culls then pick each rock record's LOD level (2: the one renderer
  change), and a type without a billboard must still hand over to the far volume (`treeCullMainPiece` only applies
  the volume's start to billboard types today).

Determinism rule as for trees: **keep `expandChunk` and the far splat's record expansion in step** - rock
variant, scale, yaw, tilt, satellites from the same hashes.

## 7. The far volume (R5)

**BUILT (2026-10-06, user-untested). As built: RendererVK CONTEXT "Far-tree volume", ROCKS; Procedural CONTEXT "World
mode", ROCKS IN THE FAR VOLUME.** Where the build differs from the plan below:

* **A rock is a SOLID with ONE extinction at any size** ("Trees/Far rock extinction (1/m)", 4): its grid is occupancy
  (16^3 from the SDF), not an extinction that thins with scale as a crown's does. Its far mass grows with scale^3.
* **No per-column albedo sums.** The trees keep their colour (last writer wins); the rocks add their share to ONE
  extra image (the rock sum per column), and the resolve mixes the climate's bedrock in by the rock fraction and stores
  1 - the fraction in the colour map's alpha. The bedrock colour is computed per COLUMN in the resolve, not per rock:
  the terrain's rock materials' textures at their smallest mip (the mean - no CPU table, no bake), weighted by their
  climate boxes over every rock entry.
* **No per-type later hand-over (item 5).** Every rock hands over at the volume's start, as a tree's billboard (the
  cull: a type without a billboard drops its mesh past `Far start` + `Far overlap`), through the march's fade band. A
  per-type later start would double or gap rocks by up to the rebake distance, without a fade. If big rocks read as
  soft blobs past 600 m, that is the next step (or the blob shrink per material).
* The sink is baked into each variant's volume box; the lean (Align) is left out of the volume.
* The far records pass now takes the column's type by atomic MIN (a tree's before a rock's, deterministic).

The volume holds trees and rocks in one march. Changes (the plan):

1. **Material in the colour map.** [Certain] Today a column's colour is "last writer wins" (one RGBA8 colour).
   Rocks in the same column as trees would overwrite the canopy colour. Change: accumulate albedo x extinction and
   extinction per column (two more R32UI fixed-point sums in the splat), resolve to a mean albedo, and write a
   ROCK FRACTION (rock extinction / total) into the colour map's alpha.
2. **Lighting by the fraction.** Rock: no forward scatter, no leaf transmission through the "crown" sun taps, a
   lower interior shadow (a solid rock has no lit interior). Blend each term by the fraction. The march has 72
   registers today [Certain, CONTEXT 2026-10-05]; measure the register count after this change.
3. **Rock albedo comes from the climate, not the type.** A table of the mean albedo of each bedrock material
   (the terrain texture bake takes each `diffr` mean at load), and the splat picks the material by
   `pickClimate(terrainClimateAt(record xz))`. Climate is km-scale smooth, so the far cascade's coarse texels
   are enough. [Likely] the far cascade covers the whole `Far end` range - check its world size.
4. **Vertical size.** The layer is `Far height` 22 m over 10 slices (~2.2 m each) [Certain, CONTEXT]. A 2 m
   boulder fills about one slice; a 20 m boulder fills the layer. A rock taller than the layer is moved down
   into it, as a too-tall tree is - that is the reason for the ~20 m limit in 1. Small rocks: at 500 m a 2 m
   boulder is ~5 px at 1440p [Guessing: 60° vertical FOV], beyond 2 km under 2 px - only the field's mean colour
   and darkening matter there.
5. **Big rocks get later hand-overs.** A 20 m boulder at 500 m is ~50 px: too large for a soft blob. Per type a
   far-start scale (the trees' `Far distance scale` idea, per type): big rock types keep their mesh tiers out
   to ~1.5-2 km, and the volume starts there for them (the cull's hand-over test per type, the splat skips a big
   rock inside its own start).
6. **Floor.** Rocks floor a column as trees do (the dominant-cover rule). A rock can set the floor of a column
   that a tree also covers; both use the same floor, so nothing changes in the march.

Visual risk [Guessing]: a soft blob reads as a bush, not as a rock. If it does, use the per-material
`blob shrink` (a harder edge for the rock fraction) before any other change, and A/B it with the user.

## 8. Shadows and RT

* Sun shadow: the LOD chain casts, as any mesh - the shadow cull picks its own, coarser level (RendererVK "Mesh
  LODs": stateless, 4x the error / +2 levels).
* RT: the chain shares ONE BLAS at `RT/BLAS LOD level`, as any chain. The preview's rocks are raytraced since R3.
* `shadowDistance` per type (as bushes) stays available for the smallest types if the far cascades fill up.

## 9. Phases

| Phase | Content | Done when |
|---|---|---|
| **R1** (built 2026-10-05) | `.rock` loader + SDF generator + meshes; a PREVIEW field (like the tree grove: a `Rocks/Enabled` tweak, a row per type, a rock per variant in front of the camera) | the user can judge the SHAPES |
| **R2** (built 2026-10-05, user-untested) | the rock material: the `LitRock` variant, world biplanar of the climate bedrock pick, snow / ground cover, contact band, wetness; the climate from `terrainClimateAt` per vertex | rocks match the cliffs beside them |
| **R3** (built 2026-10-05, user-untested; was the tessellated route) | the regular mesh LOD chain per variant (level 0 at `Lod` triangles, the GPU's per-instance pick), the per-vertex CAVITY (AO + the ground cover in crevices), the preview raytraced through the chain's BLAS | LOD switches do not show; crevices read darker |
| **R4** (built 2026-10-05, user-untested) | world placement: rock records in `TreeWorld` (a world lattice per type, crag / talus fit, the halo for the neighbours' rocks, trees rejected inside rocks); rock types in the world's instance set, with the culls' LOD pick for rock records and the TLAS fallback (2). Not built: satellites | rocks in the streamed world |
| **R5** (built 2026-10-06, user-untested) | far volume: albedo accumulation + rock fraction, rock lighting in the march, record splat for rock types (detail + far mass passes) | boulder fields visible to `Far end` |
| **R6** | remove the `boulder` / `rocks` scatter rules and their assets from `Scattering.cpp` | one rock system |

R1-R3 can be judged by the user without any world integration (the preview). R4-R5 follow the tree path step by
step, so most of their risk is already known.

## 10. Open questions (for the user)

1. **Gameplay.** Do boulders block units in `Code/Game` (Nav obstacles, physics colliders from a coarse LOD level)?
   This changes R4: records near the play area would need CPU colliders. Default in this plan: visual only.
2. **Large outcrops.** Rocks above ~8-10 m (tors, mesas) are near terrain scale. Keep them out of this system
   (terrain's job) or allow a `Huge` size class with its own tier distances?
3. **Low-level normal bake.** The coarse LOD levels shade with their own vertex normals plus the bedrock normal
   map. If a coarse level looks "blobby" before its switch distance, bake an object-space normal map from the full
   mesh per variant - one more texture per variant. Decide after looking at R3.
4. **Shape vs climate coupling.** The plan lets a type carry an optional climate box (basalt columns only in the
   warm + wet box). Alternative: types are fully climate-free and only the bedrock material changes. Which one?
