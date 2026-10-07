# Ground clutter: placement and rendering plan

Status (2026-10-07): **C1-C4 and C6 built the same day; C5 partly (the grass's canopy thinning; no far
tints).** The built system is documented in Procedural CONTEXT "Ground clutter" and RendererVK CONTEXT "Ground clutter";
section 12 lists where it differs from this plan. Companion to `Docs/TreeRenderingPlan.md` and
`Docs/RockRenderingPlan.md` (whose section 1 moved pebbles and fallen branches to "a later, separate ground-clutter
system" - this one).

## 1. Goals

* SMALL objects on the ground near the camera: pebbles and small stones, fallen branches and twigs, mushrooms,
  flowers. Later: pine cones, leaf litter clumps, ferns, small dead logs.
* **Placed in a LOGICAL way** - each kind where it comes from:
  * fallen branches and twigs under and near trees, of the SPECIES that stands there (oak branches under oaks);
  * mushrooms in damp shade under the canopy, in rings and small groups, near trunks and fallen wood;
  * pebbles around big rocks (a debris apron), at the foot of slopes, on scree and on dry stream beds, rare on
    meadow;
  * flowers in OPEN grass (not in forest shade), in drifts of one species, by climate.
* **Flowers sway with the wind** exactly as the grass around them does.
* **Per-frame cost first** (the user, 2026-10-05: load / chunk cost matters little). Densities of thousands per
  hectare must be cheap.
* Visual only: no collision, no Nav, no network sync.

Out of scope: leaf litter as a TEXTURE (that is terrain-shader work - see 9), grass changes beyond the canopy
thinning in 4.3, interaction (trampling, kicking pebbles).

## 2. Why not the current `ScatterSystem`

`ScatterSystem` (Procedural CONTEXT "ScatterSystem", off by default) is the wrong shape for clutter [Certain, from
the code]:

* One CPU `RenderNode` per instance, pushed on the main thread every frame, plus a `SpatialEntry` per cell group.
  At clutter densities (500 .. 5000 / ha, within ~80 m: ~2 ha in range) that is 1 k .. 10 k nodes per frame of
  main-thread work, for objects of a few hundred pixels.
* It places imported `.oc` models with a CPU dart throw per cell. Nothing in it knows where the trees or the big
  rocks are (they are TreeWorld records now), so "branches near trees" cannot be expressed.
* Its rules are a C++ table (`scatterAssets()` / `scatterRules()`), not data files.

**Recommendation: a NEW, GPU-driven system, and `ScatterSystem` removed at the end** (its `boulder` / `rocks` rules
are already replaced by rock records - Rock plan R6). Decision for the user - see 10.1.

## 3. The model: stateless GPU placement in patches, like the grass

The grass already does what clutter needs (RendererVK CONTEXT "Procedural grass"): a world grid of PATCHES around the
camera, a fixed list of RANKED candidates per patch (an R2 low-discrepancy sequence, so every prefix is evenly
spread), density from the terrain's own splat logic, distance thinning by rank, objects that SHRINK into the ground
instead of popping. **No CPU memory, no records, no per-frame CPU work.** Clutter copies that model:

* **A clutter patch** (`Clutter/Patch size`, ~4 m) holds `CLUTTER_CANDIDATES` (~64) ranked candidate points.
* For each candidate the cull computes the DENSITY of every clutter type at that point (5), sums them, and keeps the
  candidate where `(rank + 0.5) / N < summed density x thinning(distance)`. The type is picked by share, as
  TreeWorld picks a species (a hash of the patch and the rank).
* Variant, yaw, scale, the slight lean: hashes of (patch, rank), as `treeRecordSeed`. The ground height: the
  terrain chunk mesh, as the grass roots (`grassGroundHeight`) - the baked height map is too coarse for an object of
  10 cm.
* Revisiting a spot gives the same clutter: everything is a pure function of the world position and the seed.

Why per patch and not per object records: the user rejected per-blade records for grass (memory
[[grass-rendering-plan]]); clutter has the same scale problem one level up. A record pool (TreeWorld style) would
hold ~13 k records per 256 m chunk at 2000 / ha and would have to be expanded on CPU jobs into the tree set (capacity
600 k) - the GPU can evaluate the same function where it draws.

## 4. The context: what the GPU must know at a candidate

### 4.1 From the terrain (already on the GPU)

The grass cull already reads all of it: `terrainLayers` (beach / rock / snow / ground coverages and the climate pick's
grass amount), temperature at the height, humidity, slope (the smooth chunk-mesh normal), water level / water reach,
the flow field. Clutter reads the same, through the same includes.

### 4.2 From the trees and rocks: the FOREST FLOOR MAP (new)

The tree and rock records are on the GPU already (`TreeRecordPool`: the pool, the chunk table, the toroidal chunk
map - RendererVK CONTEXT "WORLD TREE RECORDS"). A candidate cannot search them (a 256 m chunk holds thousands of
records). So a small compute pass SPLATS the records near the camera into a camera-centred 2D map, once, and the
clutter cull reads the map:

| Channel | Meaning | Splat |
|---|---|---|
| `canopy` | crown cover 0..1 (shade, litter source) | each tree: a disc of its crown radius (`TreeRecordTypeGpu` has it), soft edge, max / screen-blend |
| `trunkNear` | ~1 near a trunk, 0 at ~2 crown radii | each tree: a falloff from the trunk |
| `species` | the dominant tree type here (8 bits) | the tree with the largest weight at the texel |
| `rockNear` | ~1 at a rock's edge, 0 at ~1.5 x its radius out | each rock record: a ring falloff |
| `occupied` | 1 inside a trunk or a rock footprint (no clutter inside a boulder) | trunk radius, 0.9 x rock radius (TreeWorld's rejection rule) |

* Size: ~512² texels at 0.5 m = 256 m, toroidal, re-splatted only for the rows / columns the camera moves into and
  when record chunks change (the far volume's re-bake trigger). Trees within the map: a few thousand. **Cost: well
  below 0.1 ms per update** [Likely] - the far volume's detail splat does far more work per bake.
* The splat EXPANDS each record as `expandChunk` does (variant, scale, yaw: the same hashes - `tree_record.inc.glsl`),
  so the canopy disc matches the drawn crown. **One more place to keep in step** with `expandChunk` /
  `tree_volume_splat.cs`.
* The BUSHES of a tree (`Bushes per tree`) also go into `canopy` with their own radius: twigs and mushrooms under
  bushes too.

### 4.3 A gift for the grass

The grass cull can read `canopy` too: thin the grass under a closed canopy (a real forest floor has little grass),
in a ring of litter round each trunk. One multiply in `grass_cull.cs.glsl` - optional, a tweak (`Grass/Canopy
thinning`). Decision for the user (10.4).

## 5. The clutter type asset (`.clutter`, `Assets/Clutter/*.clutter`)

The same pattern as `.tree` / `.rock`: the type sets the SHAPE; `Placement` blocks (any number, densities ADD) say
where. The ground terms are new and are the "logical" part.

```
ClutterType <name>
	Kind Pebble|Branch|Mushroom|Flower
	Seed n · Variants n · Scale min max (m)
	Lod triangles (level 0; each next a quarter, as rocks)

	# Kind-specific shape (6):
	Pebble:   the .rock SDF keys (Aspect, Squareness, Erosion, Noise, ...) - a small RockType
	Branch:   Species <tree name> | Any   (a module of that species' piece library; "Any" = the canopy's species)
	          Length min max · Twigs n · Decay 0..1 (bark loss, darker)
	Mushroom: Cap radius height · Stem radius height · Shape Dome|Flat|Cone|Funnel · Color r g b · Spots n
	Flower:   Stem height · Head Daisy|Cup|Spike|Umbel · Petals n · HeadSize m · Color r g b (+ Center r g b)
	          StemsPerClump min max

	Placement
		Density (per ha at full fit)
		Temperature min max · Precipitation min max · ClimateWidth   (the .tree / .rock climate box)
		Slope ... · Altitude min max                                    (the .rock bands)
		# THE FOREST FLOOR (multipliers, default 1 = does not care):
		Canopy  open shaded          (density in the open / under a full canopy: branches 0 1, flowers 1 0)
		Trunk   x                    (extra near a trunk: mushrooms 3, branches 2)
		RockNear x                   (extra at a rock's foot: pebbles 6)
		Species <tree>...            (only under these species: pine cones under Pine)
		# THE GROUND (from the terrain, 4.1):
		Grass   bare full            (by the climate pick's grass amount: flowers 0 1)
		Crag x · Talus x · Scree x   (pebbles on scree and below cliffs)
		Wet     dry wet              (by humidity / water reach: mushrooms 0.2 1)
		Stream  x                    (near flow-field channels and water's edge: pebbles 3)
		Cluster size coverage        (patches: flower drifts, mushroom groups)
		Ring    radius               (mushrooms: a fairy ring - density on a circle of noise-varied radius)
```

* A BRANCH follows the trees: `Species Any` takes the floor map's `species` channel, so the branch under an oak is
  an oak module (bark texture, form) and under a pine a pine module [Likely - needs the bark materials of all species
  resident, which the world set already holds].
* Placeholder types to start: `Pebble`, `Flint` (flat), `Twig`, `Branch`, `PineCone`, `Bolete`, `FlyAgaric` (red,
  spots), `Chanterelle`, `Daisy`, `Poppy`, `Lupine` (spike), `Cornflower`, `Heather` (cold wet).
* The type list registers like the rocks: name-sorted, an 8-bit index; loaded by a `ClutterSystem` in Procedural.

## 6. Meshes: generated, from the systems that already exist

| Kind | Generator | LODs |
|---|---|---|
| Pebble | `RockGenerator` (the SDF + surface nets + `meshopt_simplify`), small `Lod` (~150 tris) | 3 levels, the engine chain (`createMeshLodChain`) |
| Branch | `TreeGenerator`: ONE branch module of the species (`meshPiece`), cut, laid flat, the leaves dropped (or a few, brown) | the piece LODs the tree path has |
| Mushroom | new, small: a lathe (stem + cap profile), 6..16 segments | 2-3 levels |
| Flower | NONE - built in the vertex shader (7) | the grass LOD rows |

* Pebbles shade with the ROCK MATERIAL (`LitRock`: the climate's bedrock pick), so a pebble under a sandstone cliff
  is sandstone, with no per-type colour [Certain that the material exists; Likely that it works unchanged at this
  scale - the biplanar texture scale may need a per-instance factor].
* Branches shade with their species' bark (the tree path's materials), darkened by `Decay`.
* Mushrooms: `LitOpaque` with a per-type colour and a procedural spot pattern; no texture.
* All meshes are generated at load on jobs (the rock path's "Rock generate" pattern), cache is optional.

## 7. Flowers: inside the grass pipeline

Flowers need the grass's wind, its ground, its LODs and its density, so they are a SECOND DRAW of `GrassPipeline`,
not a clutter mesh:

* **The stem is a blade.** The same quadratic Bezier, root on the chunk mesh, the same lean + `vegetationWind` +
  ripple, the same geomorph - built by the same functions in `grass.inc.glsl`. Thinner (a few mm), taller per type.
* **The head is built from the vertex ID** at the stem's tip, in the tip's frame (the tangent at the tip): `Daisy` =
  a disc of N petal quads around a centre, `Cup` = petals tilted up, `Spike` = small quads up the last third of the
  stem, `Umbel` = a flat cluster. ~8..24 vertices, no vertex buffer (the grass's "vertex ID = blade << shift |
  vertex" index buffer). The head nods with the tip, so the wind moves it for free and the motion vectors use the
  grass's last-frame path.
* **Placement:** the clutter density function (5), only the `Kind Flower` types, evaluated in the GRASS CULL per
  patch - the same ranked prefix over a second, smaller candidate list per patch (~16). Flowers grow where the grass
  grows (`Grass` term), so the cull has the inputs already.
* **Colour:** per type (`Color`, `Center`), + per-flower variation; the petals two-sided with transmission (thin).
  Far: the flower drifts tint the far grass / terrain shading (9).
* **Shadows:** the near grass cascade draws them as it draws the blades (`GRASS_NEAR_SHADOW`); the canopy model
  counts them as blades.

The range is the grass's (145 m), the heads shrink with the blades. One cost to measure: the grass FS gets a
second shading branch (petal vs blade) or a second pipeline - prefer a second pipeline variant [Likely].

## 8. Rigid clutter rendering (`ClutterPipeline`, RendererVK)

Pebbles, branches, mushrooms: real meshes with LOD chains, but too many and too short-lived for the instance stream.

* **The cull** (`clutter_cull.cs.glsl`, a compute secondary as the grass cull): one thread per CANDIDATE of the
  visible patches (patch range test + frustum first, as the grass; then per candidate: density -> keep -> type,
  variant, transform -> its own sphere frustum test -> LOD level from the projected size). It appends a small
  instance record (position, quaternion + scale as halves, type / variant / level, ~24 B) into a per-(mesh, level)
  BUCKET, and counts it - the tree culls' bucket pattern. Indirect draw commands per bucket.
* **The draw:** one `drawIndexedIndirectCount` per pipeline (rock material, bark, mushroom) over the buckets; a
  small VS reads the record (instance-rate, as the grass) and the mesh from the vertex mega-buffer. The FS is the
  lit core, `LIT_NO_RTAO` as the grass (the objects are not in the TLAS).
* **Fade:** a candidate near its keep threshold shrinks into the ground over a band (the grass's `Grow band`), so
  distance thinning never pops. Per-type `Range` (pebbles 40 m, branches 80 m, big branches 120 m, mushrooms 50 m).
* **Shadows:** casters into the NEAR GRASS CASCADE (the 8 m box: real contact shadows where they are large on
  screen), plus optionally cascade 0 for the larger types (branches > 1 m). Beyond that a contact-darkening term
  at the root (as the grass's root occlusion) stands in [Likely enough - the objects are 5..50 cm].
* **No RT, no GI:** not in the TLAS (too many, too small). The TLAS writer never sees them.
* **No CPU cost per frame** apart from the UBO fields.

Budget estimate [Guessing until measured]: ~2000 visible patches x 64 candidates = 128 k cull threads (< 0.1 ms);
20 k .. 50 k drawn objects at ~20 .. 150 triangles = 1 .. 4 M triangles, most in the coarse levels; target < 0.5 ms
main pass at 1440p.

## 9. Far: what replaces the objects past their range

Single small objects are invisible far away; the effect of MANY is not:

* **Flower fields** tint the grass far tier (the grass memory's planned G2: the terrain shading takes over the grass
  look) by the expected flower colour coverage of the climate - so a meadow in bloom stays coloured to the horizon.
* **Forest floor:** a darker, browner ground under the canopy (litter) - the terrain FS reads the floor map's
  `canopy` near, nothing far (the far trees hide the floor). Optional.
* **Scree / pebble aprons:** already read as the terrain's scree texture.

These are small terrain-shader additions; they come after C4.

## 10. Phases

| Phase | Content | Done when |
|---|---|---|
| **C1** (built 2026-10-07) | the forest floor map (4.2) - as built a CPU job splat of the records (12); no debug view | the map overlays trees and rocks correctly |
| **C2** (built 2026-10-07) | `.clutter` loader + mesh generators (pebble via RockGenerator, a generic branch tube, mushroom lathe); no preview row | the user can judge the SHAPES |
| **C3** (built 2026-10-07) | `ClutterPipeline`: patches, the density function with every Placement term, the cull + buckets + indirect draws, near-cascade shadows | pebbles, branches, mushrooms in the world, placed logically |
| **C4** (built 2026-10-07) | flowers (stem = the grass blade, procedural heads; a kind of the clutter cull - 12) | meadows with flowers that sway with the grass |
| **C5** (partly) | grass canopy thinning (built); far: flower tint in the grass far tier, litter tint under the canopy (not built) | no ring at the range edge |
| **C6** (done 2026-10-07) | remove `ScatterSystem` (Scattering.ixx / .cpp, Settings.Scatter, the `Globals::scatter` call in main) | one placement path for the procedural world |

C1 and C2 are independent and can be judged on their own. C3 needs both. C4 needs only the density function from
C3 (it can start with the climate + grass terms and take the floor map after C1).

## 11. Decisions (the user, 2026-10-07)

1. **`ScatterSystem` is removed** (C6, done).
2. **Flowers: "a wide variety, whatever is best"** - procedural heads built in the vertex shader (four head kinds,
   any petal count, open angle and colours per type), no textures.
3. **Shadows: the near grass cascade only.**
4. **Grass under trees: "if you want"** - built as "Grass/Cover/Canopy thinning" (0.3 since the user's tuning, 2026-10-07).
5. **Branches: one generic deadwood set** (per-type colours), not the tree species' modules.
6. **Leaf litter: later, if wanted.**

## 12. As built - where it differs from this plan

* **The floor map is baked on the CPU** (a Low job in Procedural ClutterSystem, from TreeWorld's CPU records), not by a
  GPU compute splat from the record pool: the CPU already holds the records near the camera, the bake runs only when the
  camera moves ~40 m or the records change, and the renderer needs no mirror of the record types' crowns. Its channels:
  canopy, trunk proximity, rock proximity, occupied (no species channel: decision 5 made it unneeded).
* **The flowers are a type kind of the CLUTTER cull and draw**, not a second draw of GrassPipeline. What they share with
  the grass is what matters for the look: the stem IS the grass blade (`grass_wind.inc.glsl`: the same Bezier, lean and
  wind), the ground table, the grass's stem colours and the near grass cascade.
* **Placement blocks: any number per type**, each its own GPU type (their densities add), as the rocks' blocks.
* **The rigid draw is one indexed indirect draw per (mesh, LOD) bucket** (cull -> prefix -> scatter), not one per object.
* Not built: the `Stream` term (no flow field read yet), `Species` (decision 5), pine cones, the far tints (9).
