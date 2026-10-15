# Ground clutter

> Part of the Procedural documentation ([`Docs/Context/Procedural/CONTEXT.md`](CONTEXT.md)) — read that first.

## Ground clutter

"Clutter" tweaks (Settings.Clutter). Built 2026-10-07: pebbles, fallen
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

### `.clutter` - type asset (`Assets/Clutter/*.clutter`, `loadClutterType`)

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
	FLOWER:   Head Radial|Spike|Umbel|Bell|Reed|Tuft|Grain|Fern|Rosette|Leafy · Petals n · Open degrees (Radial: the petals' tilt up; below 0 swept
	          back) · PetalWidth (x HeadSize) · Stem m · HeadSize m · PetalShape Round|Notched|Pointed|Fringed|None
	          (Radial: the petal outline texture - poppy / daisy / gazania / cornflower; None = the plain tapered quad;
	          Umbel / Spike / Bell: textured florets unless None).
	          Color = petals, Color2 = the centre. NO MESH: the
	          vertex shader builds it (RendererVK "Ground clutter", the flowers). REED: the petals are long, nearly
	          pointed LEAVES (the stem's colours; Open = their lean out from upright, PetalWidth x HeadSize wide) in 3
	          TUFTS side by side (the first around the stalk, the others 0.15-0.25 x the height beside it; at most 16
	          leaves near, 8 / 4 further out), the centre a CATTAIL spike HeadSize long and a tenth of it wide on the stalk's top (Color2). TUFT
	          (a sedge): no stalk, fine leaves of 3 slots each near and 2 further out (the bend's segments: 5 / 4 / 2
	          leaves at most), each a Bezier with its control
	          straight up - out of the root as a bundle, then arching out by Open, the tip at least 15 % of its length
	          above the ground - Stem tall, PetalWidth x HeadSize wide, in Color. Neither shrinks with the grass cover
	          (a bank is bare beach). GRAIN (wild wheat / barley / oat; 2026-10-13): ONE STALK at every LOD (Petals
	          is ignored: a tuft lost stalks with the LOD's fewer slots and popped; 4 slots: two stalk halves, the ear, the
	          awns; no main stem), Stem tall (x 0.82
	          .. 1.05), `Ear Dense|Slender|Panicle` (the texture's kind, see THE FLOWER TEXTURES), the ear HeadSize long and PetalWidth x HeadSize wide (Color), nodding by Open (0 upright .. 90
	          hanging), `Awns` 0..1 = a thin bristle 0 .. 2 x the ear past it (the record's spots slot); stalks and awns
	          Color2 (straw). A stalk's top is as wide as the head's rachis (`CLUTTER_GRAIN_STALK_HALF_WIDTH` x the
	          ear's width - the same constant sizes the rachis in the texture, in Color2 unshaded), 1 / 0.7 of it at the
	          root: the head continues the stalk. FERN: Petals FRONDS from one crown over the LARGE topology (`CLUTTER_FLOWER_BIG_PETALS`
	          32 / 18 / 8 slots: 4 / 3 / 2 quads per frond, so 8 / 6 / 4 fronds), Stem long, leaning out by Open, arching
	          over with the tip drooping. TEXTURED (see TEXTURES below): a strip 2 x HeadSize wide with the frond in it - a
	          stipe (the first 15 %), 12..18 pinna pairs (by the Seed), the longest at ~40 %, each cut into pinnules
	          (PetalWidth = how much of a pinna's width the notches keep, 0 = to its midrib). Without a texture (fallback):
	          a bare stipe (the first fifth), then pinnae up to HeadSize per side - every quad
	          narrow at its base (PetalWidth = how wide the notch stays) and wide at its top: a SAW-TOOTHED edge. Color
	          (the base darker). ROSETTE (dock, plantain): Petals broad LEAVES (2 slots each: narrow base, widest at 40 %,
	          a point) lying round the root, HeadSize long, raised by Open, + the stalk (Stem) with a seed spike on its top
	          quarter (Color2). LEAFY (nettle, goosefoot): Petals leaves in OPPOSITE PAIRS up the stalk (each pair turned a
	          quarter; HeadSize at the bottom, ~half at the top), raised by Open, + a spike on the top tenth (Color2).
	          Ferns and the two weeds do not shrink with the grass cover either (shade, bare and trampled ground)
	Placement (ANY NUMBER: each is its own GPU type, their densities add)
		Density (per m^2 at full fit) · Temperature min max (C) · Precipitation min max (mm/yr) · ClimateWidth
		Cluster size coverage (m, 0..1: drifts / groups) · Slope max (rise / run) · Altitude min (m above the water)
		THE TERMS - each `a b`: the density x mix(a, b, its measure 0..1); one value = both; default 1 1 (no matter):
		Grass bare full (the GRASS AS DRAWN: the terrain's cover x the blades' clumps / bare spots x the canopy thinning;
		        every flower type has bare 0: no flower where no grass) · Crag none full (bedrock showing) · Beach none full ·
		Canopy open shaded (crowns overhead) · Trunk far near · Rock far near (a rock's foot) · Wet dry wet (humidity) ·
		River none full [curve] (the river influence: 1 in a channel or over a lake, 0 past the floodplain - linear
		        across it, so ^ curve first: 4 keeps a type close to the water) · Flow slow fast
		        (the river's speed, 0.5 .. 2.5 m/s; a lake is slow) · Water dry under (under a river's / lake's water:
		        a bank vs the middle) - all three from the terrain vertex's UV (below)
		Ring radius width cell chance (fairy rings: the density x 1 on a ring of radius +-30 %, falling off over width;
		        one ring per `cell` metres with `chance`. 0 radius = none)
```

Snow removes everything; so does the floor map's "occupied". Placeholders (2026-10-07): stones
`Pebble` (everywhere a little, many at a rock's foot, on bedrock and the beach), `Flint` (flat, broken: dry stony
ground); wood `Twig`, `Branch` (under the canopy only, more at the trunks); mushrooms `Bolete`, `FlyAgaric` (red,
spots, cool forest), `Chanterelle` (groups, damp), `InkCap` (meadows), `FairyRing` (rings in the grass); flowers
`Daisy`, `Poppy`, `Cornflower`, `Buttercup`, `Dandelion` (radial), `Lupine`, `Heather`, `Lavender` (spikes),
`Yarrow` (umbel), `Foxglove` (bells, forest edge), `Bluebell` (bells, UNDER the canopy), `Gazania` (savanna),
`Gentian` (alpine). By the water (2026-10-09): `Reed` (cattails at the edge of SLOW water, a few in the shallows),
`Sedge` (low arching fountains on the banks, `Head Tuft`), `RiverStone` (rounded cobbles in the bed, many where the water is FAST); `Pebble`
keeps out of river beds (`River 1 0.1`: the bed takes the beach layer it favours). Grains and weeds (2026-10-13):
`WildWheat` (`Ear Dense`: stout ears, fanning awns; warm dry grassland), `WildBarley` (`Ear Slender`: a brush of
very long awns, greener), `WildOat` (`Ear Panicle`: tall, a loose panicle of hanging spikelets) - `Head Grain`, all
near upright (`Open` 5 / 10 / 5: a large nod kinks at the stalk's tip and reads as broken); `Nettle` (`Head Leafy`: damp, rich ground, the edge of a wood, near the trunks), `Dock` and
`Plantain` (`Head Rosette`: damp grassland / short, trampled grass); ferns `Bracken` (big spreading stands, clearings and
open woods) and `LadyFern` (a fine shuttlecock on the damp, shaded floor near the trunks) - `Head Fern`.

**THE FLOWER TEXTURES** (2026-10-13; ClutterTexture.cpp `generateClutterTexture`): a FERN's frond (256^2), a RADIAL
head's petal (128^2: the `PetalShape` outline, fine veins), a GRAIN's ear (256^2: ONE strip over the ear and its awns -
the ear the first 1 / (1 + 2 x Awns), spikelets alternating either side of the rachis; 2..3 awns ~2 texels wide from
each spikelet's tip, 75..100 % of the awn length, FANNING OUT evenly over -32..32 degrees from upright (not split by
the spikelet's side: that left a V with a gap down the middle; set in the strip's
physical proportions; an awn that would leave the strip is shortened) - the strip is `CLUTTER_GRAIN_STRIP_WIDEN` (5)
x the ear's width, the ear drawn in its middle; 512^2 so the ear keeps ~100 texels across). The Grain key `Ear`
picks the kind: `Dense` (wheat, the default: that), `Slender` (barley: 24..32 smaller spikelets, three long awns each at
-13..13 degrees - a silky brush), `Panicle` (oat: no ear - a stout rachis tapering up, 4..5 levels of 1..2 thin pedicels
arching out (25..50 degrees) and over, ONE spikelet hanging from each end and the rachis's tip - 7..11 in all, each two
long pointed glumes in a narrow V - some with a bent awn; Awns 0, PetalWidth x 5 = its width per HeadSize), an UMBEL's floret quad (128^2: 5..7 five-lobed florets with Color2 centres - the
quads overlap into a lacy plate), a SPIKE's floret (128^2: a pea flower - round banner, keel and wings toward Color2),
a BELL's floret (128^2: a tube flaring to a five-lobed mouth). R = shading, G = a mask toward Color2 (the awns, a fern's rachis), A = cover. Made
on the type's generation job with TreeLeafTexture's coverage-preserving mips, uploaded in `finishGeneration`
(`Renderer::createTexture`), freed after the next `setClutterAssets` on a reload. Reed, Tuft, Rosette and Leafy have
none; `PetalShape None` turns a Radial / Umbel / Spike / Bell texture off.

**THE RIVER DATA ON THE GROUND** (TerrainGenerator; the terrain vertex's UV, which the terrain does not use as UV):
u = `TerrainPoint::river`, v = `riverSpeed` (m/s: the Manning speed of the most influential piece, as the water
ribbons'; 0 off rivers, on lakes and dry beds), NEGATIVE under river or lake water: -(speed + 0.01).
`grassGroundRiverData` (grass.inc.glsl) reads it over the drawn triangle: (influence, speed, under the water 0..1).

### The meshes (`generateClutterMeshes`)

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
