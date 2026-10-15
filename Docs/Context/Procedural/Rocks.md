# Rocks

> Part of the Procedural documentation ([`Docs/Context/Procedural/CONTEXT.md`](CONTEXT.md)) — read that first.

## Rocks

"Rocks" tweaks. **In development** (tiers, phases R1..R6). Status: R1
(2026-10-05; the shapes had a first round with the user): the `.rock` loader, the SDF generator and a PREVIEW; R2
(2026-10-05, user-untested): the climate rock material (RendererVK); R3 (2026-10-05, user-untested): a regular mesh
LOD chain + the per-vertex cavity (the tessellated route was dropped - the user's decision); R4 (2026-10-05,
user-untested): rocks in the WORLD - records in TreeWorld, pieces in TreeSystem's world set (see "World records" /
"World mode" under Trees ([Trees.md](Trees.md))); R5 (2026-10-06, user-untested): rocks in the FAR VOLUME (World mode, ROCKS IN THE FAR
VOLUME). "Rocks/Enabled" is on by default since 2026-10-06. Rocks are BIG objects (~1-22 m); small
stones are the GROUND CLUTTER (see "Ground clutter" ([Clutter.md](Clutter.md)): its pebbles reuse the rock SDF generator). The type sets the SHAPE only; the colour comes
from the climate's terrain bedrock material (the rock material, R2). **DEAD WOOD** (2026-10-07, the user's request: "Dead /
fallen tree Rock types") is rock types too - `Shape Trunk` + `Surface Wood` ("DEAD WOOD" below): fallen logs, stumps
and snags through the same records, LOD chains, far volume and placement rules, wearing a tree species' bark texture
(the user, 2026-10-07: "just use a wood texture instead, can use an existing one" - no procedural wood noise).

### The model: an SDF per variant, meshed into a regular LOD chain

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

### `.rock` — type asset (`Assets/Rocks/*.rock`, `loadRockType`)

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
	Placement (a world RULE - "ROCK RECORDS" under Trees ([Trees.md](Trees.md)). ANY NUMBER of blocks: their densities ADD, so a type gets
	        a rule for everywhere plus one per climate it is common in. A block without a Density is dropped)
		Density (per ha at full fit)
		Temperature min max (C) · Precipitation min max (mm/yr) · ClimateWidth (the rule's ideal climate box, as a
		        .tree's; default: every climate. The savanna = the Acacia's box, 20 30 / 300 900)
		Slope min max (rise / run UNDER the rock, a hard band) or Slope none full full none (soft edges: none below
		        the first, full between the middle two, none above the last - a boulder rolls off a slope:
		        `Slope 0 0 0.12 0.5`)
		Altitude min max (m above the local water)
		Crag (0..1: only where the terrain shows bedrock) · Talus (0..1: only at the foot of a steep slope)
		THE GROUND AROUND THE ROCK - density multipliers, default 1 ("ROCK RECORDS" ([Trees.md](Trees.md)) has the measures):
		Plains x (flat ground: nothing steep within ~20 m, "Rocks/World/Rugged slope start / full". 0: never there)
		Rugged low high (rugged ground: at the LOW end of the heights within ~20 m - a cliff's bottom, the foot of
		        a hillside - and at their HIGH end - a cliff's top, a crest. One value: both)
		Valley x (low ground with higher ground within ~160 m - a valley's floor, the foot of a mountain;
		        "Rocks/World/Valley relief start / full (m)". IN a valley the multiplier is x, whatever Plains and
		        Rugged say. Default: no valley rule)
		Forest open forest (x mix(open, forest, the trees' own density there / 60 per ha): dead wood lies under trees.
		        One value: both; default 1 1)
		River none full · Flow slow fast (the river influence and its speed, as the clutter's terms; `RendererVKLayout::RIVER_FLOW_SLOW /
		        FAST`). A rule with a River term other than 1 1 may place IN a river's bed ("Terrain/Rivers/Vegetation
		        clear" keeps every other rule out); with an Altitude min below 0 under its water. `RiverBoulder`: fast water
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

### DEAD WOOD (`Shape Trunk`, `Surface Wood`; 2026-10-07)

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
  (World records, ROCK RECORDS ([Trees.md](Trees.md))); the clutter's floor map treats dead wood as a TRUNK (FloorType kind 3: the trunk
  proximity around the capsule - mushrooms and fallen branches gather there - and occupied inside it), a rock as
  before. `RockFootprint::flat` (the widest horizontal extent / the longest axis) scales `groundTransform`'s footprint
  (a snag's is its thin base, not its height - this also halves Pinnacle's and Monolith's, which sank too deep on a
  slope).
* **WHERE** (`Forest`, ROCK RECORDS ([Trees.md](Trees.md))): the trees' own expected density at the record.

### A rock on the ground (`RockSystem::groundTransform`)

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

### The preview

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
`Rugged` / `Valley` mean - `RockWorldDesc`, see ROCK RECORDS ([Trees.md](Trees.md)); every change regenerates the record chunks); the system serves the world through `worldRules()` /
`typesRevision()` / `worldTypes()` / `worldGeneration()` / `setMeshUser`. `Reload
types` re-reads the files (a new `typesRevision`); `Respawn preview` re-places in front of the camera; `Grid
resolution` regenerates the MESHES only (`clearMeshes` + `kickGeneration`: the types stay as read, so the revision and
TreeWorld's placement stay too; only the world generation changes). After the upload the variants' CPU meshes are
freed (`finishLoad`; the density grid and the shape numbers stay). RT
sees the rocks through the chain's one BLAS. (The first enables' device-lost crash, 2026-10-05, was the renderer's:
unzeroed BLAS address entries for not-raytraced meshes - RendererVK CONTEXT, `createMesh`.)
