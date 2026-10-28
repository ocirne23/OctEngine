# Ground clutter

> Part of the Procedural documentation ([`Docs/Context/Procedural/CONTEXT.md`](CONTEXT.md)) — read that first.

## Ground clutter

"Clutter" tweaks (Settings.Clutter): pebbles, fallen branches, mushrooms, flowers and small cacti. **Nothing per object lives on
the CPU**: RendererVK's `ClutterPipeline` places every object on the GPU each frame (RendererVK CONTEXT "Ground
clutter": a grid of patches, a ranked candidate list per patch and type, kept by the type's density there).
`ClutterSystem` feeds it two things:

* **THE TYPES** - every `Assets/Clutter/*.clutter` (name-sorted; `loadClutterType`, ClutterType.cpp), loaded on the
  first enabled frame and on `Reload types`. A type without a `Placement` block that has a `Density` is skipped with a
  warning. Their meshes come from one Low job per type ("Clutter generate", ClutterGenerator.cpp; `Mesh resolution`
  regenerates them), through the generated-mesh cache (`Local/Clutter/Meshes/<name>.cluttermesh`, keyed by the file's
  text and the mesh resolution). `Renderer::setClutterAssets` then takes them once: **a GPU type per `Placement` block**
  (their densities ADD; at most `CLUTTER_MAX_TYPES` = 64 blocks), the meshes concatenated (at most
  `CLUTTER_MAX_MESHES` = 256). The CPU meshes are freed after. Disabling keeps them (an enable draws at once).
* **THE FOREST FLOOR MAP** - the trees' and rocks' RECORDS (TreeWorld, through `Globals::trees.world()`; the CPU holds
  them inside "Trees/World/CPU keep radius") splatted on a Low job ("Clutter floor map", `bakeFloor`) into
  `CLUTTER_FLOOR_DIM`^2 (384^2) rgba8 texels of 1 m around the camera, MAX-blended: **R canopy** (a tree's crown: 1
  inside 0.75 x its radius, none past 1.1 x), **G trunk proximity** (1 at the trunk, squared falloff to none at
  max(3 m, 0.8 x the crown)), **B rock proximity** (1 up to 0.85 x the rock's radius, none at 1.6 x + 1.5 m - the
  apron at its foot), **A occupied** (inside a trunk + 0.15 m or 0.85 x a rock's radius: nothing grows there). The
  record types' meaning comes from `TreeWorld::floorTypes()` (kind 1 tree - or a bush placed on its own: crown / trunk radius at its species' MEAN
  scale - `.tree` CrownRadius / TrunkRadius x (1 + Flare); kind 2 rock: its footprint CAPSULE from `rockFootprint` x
  `rockRecordScale` along its yaw, as the world places it; kind 3 DEAD WOOD - a `.rock` of `Surface Wood` - counts as
  a trunk: trunk proximity around its capsule, occupied inside it, no rock proximity).
  Re-baked when the camera leaves `Floor map/Rebake distance (m)` (40; capped so the map still covers
  `Renderer::groundRange()`) from its centre, when the CPU records under it change (a signature over the chunks and
  their counts) or on a TreeWorld restart; one bake at a time, handed over by `Renderer::setClutterFloorMap`. Without
  "Trees/World/Enabled", or with clutter disabled, no map (the floor measures read 0). The GRASS reads it too:
  "Grass/Cover/Canopy thinning" (0.3).
* Main thread, after `trees.update` and `rocks.update` and before `trees.kickWorldUpdate` (the clutter is the last
  reader of the tree records). Headless: never initialized.

### `.clutter` - type asset (`Assets/Clutter/*.clutter`, `loadClutterType`)

Keys and enum values are case-insensitive; an unknown enum value takes the first one listed.

```
ClutterType <name>
	Kind Pebble|Branch|Mushroom|Flower|Cactus
	Seed n · Variants n (1..16 meshes; not flowers) · Scale min max (x the nominal size 1: m for a pebble's longest
	        axis, a branch's length, a mushroom's or a cactus's height; for a flower x its Stem and HeadSize)
	Range m (5..500 from the camera; x "Clutter/Range scale"; the patch grid caps it, ~156 m at 4 m patches)
	Sink f (0..0.9 of the height below the ground) · Align f (0 upright .. 1 follows the ground's normal; flowers: upright)
	Color r g b · Color2 r g b (sRGB 0..1) · Roughness f · Lod triangles (level 0, 32..4000; a quarter per level)
	PEBBLE:   every .rock SHAPE key (`readRockShapeKeys` - see Rocks.md): a small rock through the rock SDF generator.
	          Color TINTS the climate's bedrock (the rock material's pick)
	BRANCH:   Thickness (radius / length at the thick end) · Twigs n (0..6, lying flat) · Bend (degrees, 0..90).
	          Color = bark, Color2 = end grain
	MUSHROOM: Cap Dome|Flat|Cone|Funnel · CapSize radius height (x its height) · StemRadius · Group n (1..5 per variant,
	          each smaller, leaning out) · Spots 0..1 (white spots on the cap). Color = cap, Color2 = stem and gills
	CACTUS:   Form Barrel|Column|Pad · Ribs count depth (the .rock key: 0..40, the valleys depth x the radius) · Girth
	          (0.1..1.5: a barrel's / a column's diameter, a pad's width, x its height / length) · Group n (Column: 1..5
	          columns) · Pads n (Pad: 1..12). Color = the skin, Color2 = the spines; the areoles on the rib crests (a
	          pad: over its faces) are the shader's (cactus.inc.glsl, as the .rock Cactus's)
	FLOWER:   Head Radial|Spike|Umbel|Bell|Reed|Tuft|Grain|Fern|Rosette|Leafy · Petals n (1..48) · Open degrees
	          (-80..85; Radial: the petals' tilt up, below 0 swept back) · PetalWidth (x HeadSize) · Stem m · HeadSize m
	          · PetalShape Round|Notched|Pointed|Fringed|None (Radial: the petal outline texture - poppy / daisy /
	          gazania / cornflower; None = the plain tapered quad; Umbel / Spike / Bell: textured florets unless None)
	          · Ear Dense|Slender|Panicle (Grain only) · Awns 0..1 (Grain only).
	          Color = petals, Color2 = the centre. NO MESH: the vertex shader builds it (clutter_flower.vs.glsl;
	          RendererVK "Ground clutter").
	          REED: the petals are long, nearly pointed LEAVES (the stem's colours; Open = their lean out from upright,
	            PetalWidth x HeadSize wide) in 3 TUFTS side by side (the first around the stalk, the others 0.15-0.25 x
	            the height beside it), the centre a CATTAIL spike HeadSize long and a tenth of it wide on the stalk's
	            top (Color2).
	          TUFT (a sedge): no stalk; fine leaves of several slots each (a bend), out of the root as a bundle, then
	            arching out by Open, the tip at least 15 % of its length above the ground - Stem tall, PetalWidth x
	            HeadSize wide, in Color.
	          GRAIN (wild wheat / barley / oat): ONE STALK at every LOD (Petals is ignored: a multi-stalk tuft lost
	            stalks with the further LODs' fewer slots and popped), 4 slots: two stalk halves, the ear, the awns.
	            The ear HeadSize long and PetalWidth x HeadSize wide (Color), nodding by Open (keep it small: a large
	            nod kinks at the stalk's tip and reads as broken); Awns = a bristle 0 .. 2 x the ear past it (it rides
	            the record's spots slot); stalks and awns Color2 (straw). A stalk's top is as wide as the head's
	            rachis (`CLUTTER_GRAIN_STALK_HALF_WIDTH` x the ear's width - the same constant sizes the rachis in the
	            texture), 1 / 0.7 of it at the root: the head continues the stalk.
	          FERN: Petals FRONDS from one crown over the LARGE topology (`CLUTTER_FLOWER_BIG_PETALS` 32 / 18 / 8
	            slots: 4 / 3 / 2 quads per frond), Stem long, leaning out by Open, arching over with the tip drooping.
	            Textured: a strip 2 x HeadSize wide with the frond in it - a stipe (the first 15 %), 12..18 pinna pairs
	            (by the Seed), the longest at ~40 %, each cut into pinnules (PetalWidth = how much of a pinna's width
	            the notches keep, 0 = to its midrib). Color (the base darker).
	          ROSETTE (dock, plantain): Petals broad LEAVES (2 slots each: narrow base, widest at 40 %, a point) lying
	            round the root, HeadSize long, raised by Open, + the stalk (Stem) with a seed spike on its top quarter
	            (Color2).
	          LEAFY (nettle, goosefoot): Petals leaves in OPPOSITE PAIRS up the stalk (each pair turned a quarter;
	            HeadSize at the bottom, smaller up), raised by Open, + a spike on the top tenth (Color2).
	          Every flower shrinks with the grass cover as the blades do, EXCEPT Reed, Tuft, Fern, Rosette and Leafy
	          (bare banks, the shaded floor, trampled ground).
	Placement (ANY NUMBER: each is its own GPU type, their densities add; one without a Density > 0 is dropped)
		Density (per m^2 at full fit) · Temperature min max (C) · Precipitation min max (mm/yr) · ClimateWidth
		Cluster size coverage (m, 0..1: drifts / groups) · Slope max (rise / run) · Altitude min (m above the water)
		THE TERMS - each `a b`: the density x mix(a, b, its measure 0..1); one value = both; default 1 1 (no matter):
		Grass bare full (the GRASS AS DRAWN: the terrain's cover x the blades' clumps x the canopy thinning;
		        every flower type has bare 0: no flower where no grass) · Crag none full (bedrock showing) · Beach none full ·
		Canopy open shaded (crowns overhead) · Trunk far near · Rock far near (a rock's foot) · Wet dry wet (humidity) ·
		River none full [curve] (the river influence: 1 in a channel or over a lake, 0 past the floodplain - linear
		        across it, so ^ curve (0.1..16) first: 4 keeps a type close to the water) · Flow slow fast
		        (the river's speed, 0.5 .. 2.5 m/s; a lake is slow) · Water dry under (under a river's / lake's water:
		        a bank vs the middle) - all three from the terrain vertex's UV (below)
		Ring radius width cell chance (fairy rings: the density x 1 on a ring of radius +-30 %, falling off over width;
		        one ring per `cell` metres with `chance`. 0 radius = none)
```

Snow removes everything; so does the floor map's "occupied". Placeholders: stones `Pebble` (everywhere a little,
many at a rock's foot, on bedrock and the beach; `River 1 0.1` keeps it out of river beds, which take the beach layer
it favours), `Flint` (flat, broken: dry stony ground), `RiverStone` (rounded cobbles in the bed, many where the water
is FAST); wood `Twig`, `Branch` (under the canopy only, more at the trunks); mushrooms `Bolete`, `FlyAgaric` (red,
spots, cool forest), `Chanterelle` (groups, damp), `InkCap` (meadows), `FairyRing` (rings in the grass); flowers
`Daisy`, `Poppy`, `Cornflower`, `Buttercup`, `Dandelion` (radial), `Lupine`, `Heather`, `Lavender` (spikes),
`Yarrow` (umbel), `Foxglove` (bells, forest edge), `Bluebell` (bells, UNDER the canopy), `Gazania` (savanna),
`Gentian` (alpine); by the water `Reed` (cattails at the edge of SLOW water, a few in the shallows), `Sedge` (low
arching fountains on the banks, `Head Tuft`); grains (`Head Grain`) `WildWheat` (`Ear Dense`), `WildBarley`
(`Ear Slender`), `WildOat` (`Ear Panicle`); weeds `Nettle` (`Head Leafy`), `Dock` and `Plantain` (`Head Rosette`);
ferns `Bracken` and `LadyFern` (`Head Fern`); cacti (the hot desert only - the terrain's dune-sand climate box, 20 C up / 0..300 mm, with `ClimateWidth 0.015`
and `Grass 1 0`, so none reach the grassland) `BarrelCactus` (`Form Barrel`, 20
ribs, golden spines; more on bedrock and at rocks), `HedgehogCactus` (`Form Column`, clumps of up to 5; stony ground,
at rocks), `PricklyPear` (`Form Pad`, 7 pads).

**THE FLOWER TEXTURES** (ClutterTexture.cpp `generateClutterTexture`): a FERN's frond (256^2), a RADIAL head's petal
(128^2: the `PetalShape` outline, fine veins), a GRAIN's ear (512^2, so the ear keeps ~100 texels across: ONE strip
over the ear and its awns - the strip is `CLUTTER_GRAIN_STRIP_WIDEN` (5) x the ear's width, the ear drawn in its
middle and taking the first 1 / (1 + 2 x Awns) of its length; an awn that would leave the strip is shortened), an
UMBEL's floret quad (128^2: 5..7 five-lobed florets with Color2 centres - the quads overlap into a lacy plate), a
SPIKE's floret (128^2: a pea flower), a BELL's floret (128^2: a tube flaring to a five-lobed mouth). The grain `Ear`:
`Dense` (wheat, the default: spikelets alternating either side of the rachis, 2..3 awns each FANNING OUT evenly over
-32..32 degrees - splitting them by the spikelet's side left a V with a gap down the middle), `Slender` (barley:
smaller spikelets, three long, nearly parallel awns each - a silky brush), `Panicle` (oat: no ear - a rachis with thin
arching pedicels, one spikelet hanging from each end; Awns 0, PetalWidth x 5 = its width per HeadSize). R = shading,
G = a mask toward Color2 (the awns, a fern's rachis), A = cover. Made on the type's generation job with
TreeLeafTexture's coverage-preserving mips (`buildLeafClusterMips`), uploaded in `finishGeneration`
(`RenderTexture::create`), released after the next `setClutterAssets` on a reload. Reed, Tuft, Rosette and Leafy
have none; `PetalShape None` turns a Radial / Umbel / Spike / Bell texture off.

**THE RIVER DATA ON THE GROUND** (TerrainGenerator; the terrain vertex's UV, which the terrain does not use as UV):
u = `TerrainPoint::river`, v = `riverSpeed` (m/s: the Manning speed of the most influential piece, as the water
ribbons'; 0 off rivers, on lakes and dry beds), NEGATIVE under river or lake water: -(speed + 0.01).
`grassGroundRiverData` (grass.inc.glsl) reads it over the drawn triangle: (influence, speed, under the water 0..1).

### The meshes (`generateClutterMeshes`)

Nominal size 1, y up, the lowest point at y = 0, `CLUTTER_LODS` = 3 levels each (`Renderer::ClutterMesh`;
`RendererVKLayout::ClutterVertexGpu`: position + AO, normal + PART, and a uv - branch bark / end grain coordinates in
m, a mushroom's around / up its profile, a cactus's around / along / rib count or its pad's areole lattice):

* **Pebble** - `generateRockVariant` with the type's rock keys at `Mesh resolution` (20) x its `Resolution` cells: the
  rock chain's levels 0-2, its cavity as the AO. `finishGeneration` CHECKS every pebble mesh level (`countBadEdges`:
  edges used once = a hole, more than twice = a fold) and logs a warning per bad one. The coarse levels of a few
  variants may keep 1-2 non-manifold edges from the simplification.
* **Branch** - a bent, tapering tube along X (7 / 5 / 3 sides, 10 / 5 / 3 rings), lying on the ground (the underside
  darker), its end grain capped (part 1); levels 0-1 add the twigs (thinner tubes drooping to the ground, each its
  own stretch of the bark pattern).
* **Mushroom** - per group member a lathe (12 / 7 / 5 segments): the stem (part 0, wider at the foot), the cap top
  (part 1; the cap shape's profile, its rim curled down) and the gills underneath (part 2).
* **Cactus** - `addRibbedLathe` (part 0): a profile around Y, every ring pushed in between the rib crests as a .rock
  cactus's (`ribIndent`'s rule), 3 / 2 / 1 columns per rib, the first on a crest; normals from the surface's own
  tangents; uv = (cos, sin) of the angle, the length up the profile in base circumferences, the rib count. A BARREL is
  an ellipse's arc from where the ground cuts it (the foot ~0.75 of the widest) to the crown (10 / 6 / 4 rings); a
  COLUMN group (as the mushrooms': the first upright and tallest, the rest shorter, around it, leaning out) a cylinder
  under a quarter-circle dome. A PAD (part 1, `growPads`): flattened ellipsoids (latitude / longitude 7x12 / 5x8 /
  3x6), the base pad on its end, each next out of an earlier pad's upper rim - fanning out from its tip, lifted toward
  the sky, its face turned - a little smaller, none into the ground; uv = the areole lattice over the faces. Scaled to
  height 1, the lowest point at 0.
