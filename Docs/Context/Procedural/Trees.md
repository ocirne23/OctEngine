# Trees

> Part of the Procedural documentation ([`Docs/Context/Procedural/CONTEXT.md`](CONTEXT.md)) — read that first.

## Trees

"Trees" tweaks. `TreeSystem` loads every `Assets/Trees/*.tree` species, generates its piece library and baked
variants (through the generated-mesh cache - "The generated-mesh cache" in [CONTEXT.md](CONTEXT.md)), and draws
either the WORLD (TreeWorld's records, "World mode") or a PREVIEW grove ("The preview").

### The model: a piece library, composited per tree

A tree is NEVER generated as a whole. A species generates once into a `TreeLibrary`
(`generateTreeLibrary`, TreeGenerator.cpp): **trunks** with attach **slots**, and **branch modules** (a main
branch with its sub-branch levels and leaves). `compositeTree(species, library, seed)` then picks a trunk and
fills its slots with modules: module +Y onto the slot direction, module +Z (its "up side") toward world up,
a roll jitter, a scale to the slot length. **The composite's only randomness is `treeHash` (PCG)** — keep every
GPU mirror of it bit-exact and in step.

* Pieces carry a per-vertex **bone** index (piece-local; bone 0 = the piece root, each level-1 sub-branch of a
  module gets its own) and `TreeBone` pivots/axes — the wind payload's input.
* Modules are generated in a frame where world-up is `(0, cos φ, sin φ)` for the species' mean slot angle φ,
  so up-attraction and leaf facing are world-relative after the composite.
* Slot lengths follow the crown envelope (`Crown Shape`) — the envelope sets the silhouette, modules fill it.

### `.tree` — species asset (`Assets/Trees/*.tree`, `loadTreeSpecies`)

Keys and enum values are case-insensitive.

```
TreeSpecies <name>
	Seed n · Scale min max · Kind Tree|Bush (default Tree; a Bush grows around trees, never as one)
	Climate <name> (a Bush grows around the trees of the same climate only; a bush may list SEVERAL names -
	        `Climate Boreal BirchWood` - and grows around the trees of any of them; a tree whose climate no bush shares
	        takes every bush)
	Weight n (a Bush, 1..8, default 1: how often its climate picks it - it is listed n times in each tree's bush list,
	        which the CPU and the far volume's GPU mirror both pick from uniformly; a tree's list holds at most
	        `TREE_RECORD_MAX_BUSHES` = 8 entries in the far volume)
	Undergrowth { Bushes (per tree, the fraction by chance) · Radius (m: out to this from the tree, at least 1.5) }
	Wind { Trunk (x the trunk lean + sway, 0..4, default 1) · Branch (x the branch sway, meshes + branch cards) }
	        (a Tree; absent = "Trees/Bushes per tree" and 0.75 x "Trees/Spacing (m)"; the far volume reads it per
	        record type)
	Trunk   Count (1..64) · Height min max · Radius · Taper (tip/base) · Flare · Sink (m below the base, default 0.5)
	        · <shape> · Lobes n (0..12 ridges/buttresses around the trunk; 0 = round) · LobeDepth base top (radius
	        fractions) · LobeHeight (height fraction over which the ridges fade to the top depth) · Twist (deg over
	        the height)
	        Stems min max (1..8, default 1 1: each trunk piece grows that many stems from around one base) · StemLean
	        (deg from vertical, outward, x 0.8..1.1 per stem; 12) · StemSpread (m from the centre to a stem's base, x
	        0.6..1; 0.25) · Fork (0..0.9, default 0: > 0 = one trunk up to that fraction of the height, the stems
	        forking from its top - "THE FORK" below) · Split count depth angle span (count 2..4, default 0 = off; depth
	        1..5; angle deg; span 0.05..0.8 of the stem height; optional tipCurve -1..2, default 1; optional fill, default
	        0, 1 = FILL a half sphere: every stem's top splits repeatedly - "THE SPLITS" below) · LeaderLean (0..1,
	        default 0.3: a fork's first stem leans this x StemLean - a central leader; 1 = no leader)
	Crown   SplitLimbs count angle length (needs Trunk Split; default 0: per stem `count` side limbs `angle` deg from
	        up, `length` x the stem height, splitting like its top - "THE SPLITS" below)
	        Shape Ellipsoid|Cone|Umbrella|Column|Dome (Dome: wide from the bottom up, `sqrt(1 - h^3)` floored at 0.3 -
	        a broad flat-topped crown) · Start (fraction of height) · Radius (m reach)
	        Slots (0..128) · Fill (probability) · Angle bottom top (deg from up) · AngleVar · Leader true|false
	        Top count reach angle (0..16 limbs in a whorl at EVERY stem's / split's tip, from where its point starts,
	        reach x Radius each x 0.8..1.1, `angle` deg from up +- AngleVar (default 90: level); default 0 - a stem that
	        ends in a flat sheet, its point hidden. A species with Droop grows its limbs at `Crown Angle`, so set that
	        to the same angle)
	        BranchRadius (max module base radius / trunk radius at the slot; caps the module scale)
	        Tiers count spread (slots grouped into `count` whorls up the crown, each spread over `spread` of
	        its band; envelope and angle follow the whorl's height - layered crowns; default 1 0.35) · an optional
	        third value, jitter (0..1, default 0): each whorl's height off the even spacing by up to half a band x
	        it (the order kept) and its reach x 1 +- 0.35 x it, per stem - sheets at uneven heights and widths
	Module  Count (1..256) · Length (m, 0 = crown radius) · Radius (base / LENGTH - with Length 0 a fraction of the
	        crown radius) · <shape>
	        Level { Count (0..64) · Start · Length · LengthTaper · LengthVar (each child a random 0..this shorter,
	        0..0.95, default 0) · Angle value var · Rise (0..1: the start direction blended toward world-up -
	        strands that arch over before they hang; default 0) · Flat (0..1: the child's side direction blended into
	        the parent's horizontal side - children fan out in a flat sheet; default 0) · Radius · <shape> }  (up to 3)
	Leaves  Size (m) · Aspect (width/length) · PerBranch (0..128, per average last-level branch; other branches
	        scale it by their length) · Levels (1..4 deepest tiers that carry leaves; 1 = last level only)
	        Type Single|Cluster · Cross true|false · Style Leaves|Needles|Pinnate|Mat · ClusterLeaves (Mat: shoots per cell; Needles: side
	        shoots per branch; Pinnate: pinna pairs per compound leaf) · Shoots (Needles / Pinnate: branches /
	        compound leaves fanned from the card base, 1..5, default 1) · ClusterLeafWidth (Leaves: a texture leaf's
	        half width / its length, 0.1..0.6, default 0.24 narrow; 0.45 oval) · ClusterLeafSize (Needles: the needle
	        length; Pinnate: the leaflet length; 0.02..0.6) · NormalBend (0..1, default 0.7: card normals bent away
	        from the module root, both faces)   (Cluster only, below)
	        Align (0..1, default 0: cards stick out of the branch facing up; 1 = they lie ALONG the branch,
	        facing out of it at random turns - a weeping strand's curtain)
	        Top (0..1, default 0: a card's side direction blended into the branch's nearer HORIZONTAL side, its
	        direction's slope taken out (x this) and its stem lifted onto the branch's top by its radius + 0.1 card
	        length, and never under the LIMB (the module root, which curves up past its sub-branches): at least its
	        height at the card's distance along it + its radius + 0.1 card above -
	        cards lying LEVEL on the limbs, facing up, in the branch's sheet; blending the direction toward the
	        top instead stood every card up, and keeping a drooping twig's slope dipped the cards under the branches)
	        FaceUp (0..1, default 0: the card's FACE blended toward up. With `Align 1` a card lies ALONG its branch and
	        straddles it - its drawn spray down the middle, over the real twig - so facing up it COVERS the branch from
	        above; it is raised onto the branch's top (x FaceUp: 1.15 x the radius + 0.05 card), or it cut through the
	        wood. A card otherwise starts at the branch centre and reaches out to ONE side, so the branch always runs
	        along its edge and shows between the cards, whatever the twigs do - the cedar's failed tries)
	        Pad count width height tilt start (Cluster; default 0 = off; tilt default 0.25, the cards' random tilt off
	        level; start default 0.15: FOLIAGE PADS replace the per-branch leaves - per module
	        `count` cards of Size scattered through a flat DOME over its limb: centred at a point of the limb from `start`
	        to its end, denser toward the end (`mix(start, 1, sqrt(r))`: a limb's inner part lies in the shade of the tiers
	        above - foliage at its END, not along its whole length), area-uniform over a disc `width` x the module length
	        to either side (x 0.4 along the limb, narrowing to 0.75 toward its tip), at `height` x the module length x (1 - r^2) x 0.6..1 over it; each card
	        centred on its point, level +- the tilt, its base edge (the texture's twig) toward the limb: pointing out from it
	        +- 40 degrees about up - and a TWIG MESH to it: a thin arched tube (3 sides, level 1) from the limb to the
	        card's base edge, as thick as the texture's twig (0.011 card, 1.6x at the limb), so every card hangs on wood
	        (none within 5 cm of the limb). Not attached to any twig: a tier
	        reads as a cloud of foliage with the branches inside it - cards that follow the twigs could not. Use it
	        with `Style Mat` and `Cross true` - one spray per card read as one big leaf at pad sizes, level cards as
	        thin lines from the side)
	        Lift a b (default 0 0: cards raised along the module's up, free of their branch - a x the card Size for every
	        card, plus a DOME of b x the module length: highest along the limb, `1 - (side / 0.45 L)^2` across, faded
	        out over the limb's last 45 % - sheets that sit ABOVE the branches and taper at the edges; drooping twigs for
	        the dome left the limbs above the cards)
	Lod     ErrorScale (x the per-level error; unused while the trees upload level 0 only - "Piece mesh LODs")
	Bake    Variants (baked whole trees per species, 1..64, default 4)
	Billboard Distance (m, 0 = none; default 50) · Resolution (px, rounded up to a power of two, 16..2048; 512)
	          NormalBend (0..1, default 0) · FadeWidth (m, 1..1023, the mesh <-> billboard crossfade band centred on
	          Distance; 10)
	          AlbedoScale (x the WHOLE-TREE billboards' colour, linear, 0..8; default 1 - applied after the cache, at
	          load: a change needs no re-bake; the module billboards / branch cards and the far volume keep theirs)
	          FarAlbedoScale (x the FAR-TREE VOLUME's colour, linear, 0..8; default 1 - with or without `Color Far`)
	          TopCardHeight (the WHOLE-TREE billboard's horizontal card, a fraction of the tree's height: 0 = bottom,
	          1 = top; default 0.5 - its bake is orthographic from above, so only the card mesh moves: no re-bake)
	Bark    Plates lines rows (1..64 fissure lines per family around / horizontal-break rows along, per tile)
	        Crack (fissure width, ridge fraction) · Breakup (0 continuous lines .. 1 short segments) · Relief · Lichen
	        Lenticels (0..1: birch dashes - the share of a 6 around x 24 along lattice's cells that hold a short dark
	        dash ACROSS the branch) · Patches (0..1: about that share of the bark dark, rough patches, wider around
	        than along)
	        Sections (0..1: about that share of the bark tinted to Color Section, soft areas longer along the branch)
	Color   Bark r g b · Leaf r g b · Mark r g b (the lenticels' and patches' colour; default 0.09 0.08 0.07)
	        Section r g b (default 0.45 0.33 0.24) · Twig r g b (the twigs / stems drawn in the LEAF texture; absent =
	        Bark - a birch's white bark read wrong on its twigs) · Far r g b (sRGB like Leaf: the FAR-TREE VOLUME's colour; absent =
	        the leaf texture's alpha-weighted mean, `meanLeafAlbedo`)
	Placement (WORLD placement, TreeWorld; trees only - absent or Density 0 = never placed in the world)
	        Density (per ha in the ideal climate) · Temperature min max (C; the IDEAL range, one value = exactly that)
	        Precipitation min max (mm/yr; the ideal range - 2200 and up is the encoding's top, so a max there is no max)
	        ClimateWidth (sigma of the falloff OUTSIDE the ideal box, in the normalized climate space, 0.06)
	        Cluster size coverage (m, 0..1; size 0 = no patches) · MaxSlope (rise/run, 0.6)
	        Altitude min max (m above the local water level, 1.5 ..)
	        Spacing (m, 0..30, default 0: no other tree within max(this, its own spacing) of a tree with a higher
	        priority - "SPACING" in World records)

<shape> (TreeBranchShape, any of): Curve value var (deg, smooth bend) · UpAttract (negative droops - weakly:
        -1 over the length bends a horizontal branch only ~45° down)
        Droop (deg turned toward straight DOWN over the length, a fixed rate that stops at vertical: 400 = the
        strand hangs after a quarter of its length; 0..720, default 0). A species with ANY Droop grows each
        module for its own pitch (spread over the slot angles ± AngleVar, not the mean), and the composite
        places the nearest-pitch module AT that pitch with no roll jitter, so the module's down is exactly the
        world's (a mean-pitch module with roll jitter hangs its strands far off plumb).
        Wobble (deg random walk per segment) · Elbows (average count, 0..8) · ElbowAngle value var (deg)
        ElbowUpBias (probability a downward elbow turn is mirrored upward; default 0.5)
        ElbowMinElevation (deg vs horizontal an elbow may turn down to; default -15; a branch that already
        points lower before the elbow is not lifted) · ElbowRange start end (length fractions; default 0 1)
        Stubs (probability an elbow carries a snapped-off stub) · Rings · Sides
```

**Elbows** are sharp turns on interior ring nodes, each rounded by replacing the corner with three points of
a quadratic Bezier (+2 rings per elbow); everything along a branch is addressed by its arc-length fraction
`u`, not by ring index. A **stub** continues the direction the branch had BEFORE the elbow (the branch that
snapped off), short and thick, with a flat broken end cap.

Placeholders (trees): `Oak` (ellipsoid, cluster cards), `Pine` (cone + leader, two drooping sub-branch levels, 1 m
NEEDLE-spray cards of 3 fanned branches on all three tiers - single leaves read as bare poles), `Acacia` (umbrella,
up-attracted sub-branches, flat 0.9 m PINNATE cards on the last two tiers; leaflets 0.07 of the card - true-to-life
~2 cm leaflets are invisible), `Willow` (weeping: 20 thin limbs in TWO whorls - `Tiers 2 0.4`, an Ellipsoid envelope
(Cone gave the upper whorl far too little reach), trunk `Taper 0.35` + `BranchRadius 0.7`: the thin upper trunk caps
the upper modules - the upper layer's size knob; strands that rise, arch over the dome and hang plumb (`Rise`,
`Droop`), of random length (`LengthVar 0.65`: curtains ending at layered heights), narrow cluster cards along them,
`Leaves Align 0.8`; ~5x Oak's cards per module), `Birch` (a slender white trunk - `Bark Lenticels` + `Patches` in
`Color Mark`, brown `Sections` -, a wide, open oval crown (`Radius 4.8`, `Angle 68 38`) of limbs with hanging twigs:
`Droop` on both levels; no leaves on the limbs themselves and no twigs on their inner 30 % (`Levels 2`, `Start 0.3`),
so the leaves sit toward the outside), `BirchClump` (the same with `Stems 2 3`: thinner stems that lean apart only a little
(`StemLean 6`) and turn back up (`UpAttract 0.9`), each its own crown), `Cedar` (Lebanon cedar: a short buttressed trunk
(`Height 7.5 9.5`: shorter stems - 10.5 13 made them too long, 8.5 10.5 the tree too tall for its width; `Radius 0.65`, `Taper 0.08`) forking at 0.3 (`Fork 0.3`, `Crown Start 0.3`: bare below) into 4-5
gnarled, BENDING stems leaning apart (`Stems 4 5`, `StemLean 38`, `StemSpread 0.25`; `Curve 22 10`, `Wobble 7`,
`Elbows 1.8`, `ElbowAngle 22 8`, `UpAttract 0.2`; their heights all random - one full-height stem stood out of the
crown). Each stem's top 24 % SPLITS three times into two branches fanning out wide and upward (`Split 2 3 44 0.24`), and
7 flat SPLIT LIMBS (`SplitLimbs 7 84 0.4`, x the `Shape Cone` envelope: long low, short high - a flat cone; even
lengths made a tall, narrow tree) branch the same way lower down - the ONLY side limbs (`Slots 0`:
the plain module limbs read as spindly); every final tip curves up into a
short whorl (`Crown Top 2 0.25`) carrying a round FOLIAGE PAD from its base (`Leaves Pad 22 0.6 0.1 0.3 0.05`,
`Size 2.2`, `Cross true`, `Style Mat` - 90 shoots; FEW LARGE cards: every card hangs on its own twig mesh, and 55 small
ones over a wide pad read as a mass of spindly straight twigs; fine needles `ClusterLeafSize 0.035`, a lighter green `Color Leaf
0.20 0.33 0.15`): level sheets at many heights - clouds of foliage with the branches inside them, as in the photos.
The whorl limbs on the tips are the only modules (`Crown Radius 5.5` sets their length with `Top`; `Module Radius
0.009`, one short sub-branch level of 2); every pad card hangs on its own thin arched twig mesh. Tried and dropped: an Ellipsoid (an egg),
one straight trunk (a fir), each stem its own `Umbrella` crown (an inverted pyramid), rising limbs (a V), `Angle 112`
from `Start 0.2` (limbs to the ground), many long plain limbs (a wide skirt under narrow splits), long split limbs
(`SplitLimbs 3 68 0.4`: long bare branches up through the crown), long whorl limbs (`Top 2 0.5` with the pad from 35 %:
a bare stem sticking out level), a high fork (`Fork 0.4`, `Radius 0.9`: the trunk too large for the crown); cards ON the twigs in every arrangement - out to one side (the branch along every
card's edge), level (`Leaves Top`), lifted (`Leaves Lift`), drooping or rising-then-drooping twigs, along the branch
facing up (`Align 1` + `FaceUp`: too much overlap, it read as cut paper); pads of 14 cards (scattered leaves: a ~30
m^2 pad needs ~5-6 cards deep), pads of one Needles spray per card (each read as a big broad leaf, at any size), level
uncrossed pad cards (thin lines from the side), long thick falling limbs (`Angle 104 88`, `Radius 10`, `Module Radius
0.025`: too heavy), steeply rising limbs (`Angle 72 62`: a vase)).
`Elm` (tuned with blind screenshot tests - a context-free agent naming the species from a neutrally named shot: elm
first, ahead of oak and willow. One STRAIGHT trunk (`Stems 1`) whose top 65 % is a crown that FILLS a half sphere
(`Split 4 3 0 0.72 -0.4 1 1.8 2`, 2 leafy shoots on each inner branch; the crown from 28 % of the height, its middle raised 1.8 x - a tall rounded crown, a
half sphere read as a flat dome; `Crown Radius 7.5`: 64 tips on the dome, snaking with the trunk shape's `Wobble 14`, the
last round arching out a little), every tip ending in a whorl of 3 leafy limbs rising at 55 degrees (`Top 3 0.5 55`,
`Crown Angle 55 55`) that arch over a little (`Module Droop 60`); thin leafy limbs (`Module Radius 0.005`, ~2 cm at the base, on ~1.5 cm
split tips) with `BranchRadius 1.5` (`Module Radius 0.012` + `BranchRadius 6` put ~5 cm limbs on ~5 mm tips: a step in
thickness); broad OVAL leaves (`ClusterLeafWidth 0.55`, `ClusterLeafSize 0.27`). Tried and dropped: a fork into 2-5
stems with fanning splits and split limbs (uneven, lumpy, a long trunk or a mass in the middle - the fill replaced it),
level tip limbs (`Crown Angle 90`: straight horizontal branches), foliage only at the top (bare stems under a cap, read as acacia), narrow or many small leaves a card (read as
willow), strong drooping limbs (willow), flat split limbs (`SplitLimbs 3 65`) and long tip limbs (`Top 3 0.8`: long thin
horizontal branches), split limbs low with `Umbrella` (a mass of branches in the vase's middle), dense foliage (`Top 7`,
`PerBranch 12` on 3 levels: a solid ball, 6.7 M leaf triangles in the library and a GPU device loss)).
Climates: `Oak` Temperate, `Pine` Boreal, `Acacia` Savanna, `Willow` Temperate, `Birch` / `BirchClump` BirchWood (the bush-matching name; their placement box is a cool
band BETWEEN Pine's box and Oak's), `Cedar` Temperate (Oak's warmth, too dry for Oak) - the boxes in "World records".

**THE TRUNK'S TIP** (`generateTrunk`, `appendTube`'s `tip`): every stem's mesh narrows over its last ~6 tip radii
(`6 x Radius x Taper / height`, 0.03..0.3 of the length) to `TUBE_TIP_RADIUS` (0.12) of the path's radius - a closed
point, never an open cut. The mesh only: `BranchPath::sample` keeps the linear radius, so the slots and their module
caps are unchanged. A `Leader` grows from where that narrowing STARTS (the leader and the trunk's point taper together;
from the very end it stood on an open cut).

**THE FORK** (`Trunk Fork` f > 0, several stems): ONE trunk from the ground to f x the height (the flare, the ridges,
the sink, rings packed toward the ground; STRAIGHTER than the stems - a third of the trunk shape's curve and wobble,
no elbows: the stems' bend made it look bent), the stems growing from its top - each `Radius x mix(1, Taper, f) x 1.15
/ sqrt(stems)` thick (at most 0.85 of the base's top), starting one base radius down inside it, spread by `StemSpread`
around the fork, leaning out by `StemLean` from the base's direction, `(1 - f) x 0.7..0.95` the height long. The FIRST
stem instead CONTINUES the lower trunk: from its last ring, as thick, starting along it and bending over its first 30 %
(`bentPath`) to only 30 % of its lean, 0.85 x as long as it would be (standing near vertical, it topped the crown) - a central
leader filling the crown's middle (leaning out like the others it
left a V with an empty centre); the lower trunk ends open with no cut in view (stems that all started inside it left
its open end showing). The crown's `Start`
stays a TREE height, and it is ONE crown: the BASE trunk carries limbs too (from `Start` to the fork - the long low
ones), the slots split between it and the stems by the length of each inside the crown, and every limb's envelope and
angle (`Shape`, `Angle`) follow its height IN THE WHOLE TREE, not along its own stem (each stem its own small crown made
the stems' tops an inverted pyramid). Its slots take their angle from WORLD up, not the leaning stem (level sheets).
Each stem fills its own SECTOR of the circle: centred on its outward direction, 1.2 x its share (2 pi / stems), an
even golden-ratio fill of it - the stems' limbs fan out around the tree (each stem's own full golden circle put limbs of
neighbouring stems over each other, shading them). The base trunk keeps the full circle.
Without a fork (0, the default) the stems stand on the ground, each its own crown.

**THE SPLITS** (`Trunk Split count depth angle span`): each stem grows only `(1 - span)` of its height (as thick at
its end as the full taper puts it there) and ends OPEN, then splits into `count` branches - at even azimuths about it
(+-0.3 rad), `angle` x 0.7..1.2 off it, pulled toward up (+0.3 up before normalizing: they follow the upward direction).
The FIRST CONTINUES it: from its last ring exactly, as thick, with its sides, STARTING along the parent's direction and
bending to half the angle over its first 35 % (`bentPath`, 9 rings) - the bark carries on into a thinner branch (a
closing point beside the children read as a branch ending early; starting in the new direction tilted its first ring
against the parent's last, a wedge-shaped gap). The others start half a
parent radius inside it, `parent / sqrt(count)` thick (at most 0.75 of it), half the trunk's sides (at least 6). Each
tapers to 0.45 of its start, 6 rings, no elbows or stubs, ends open where it splits again and closes into a point only
at a FINAL tip -
and splits again, `depth` rounds, every round 0.6 x as long (`ROUND_LENGTH`; 0.8 left long bare final rounds; the first round's length makes all
rounds together about `span` x the stem height). The FINAL round curves: it leaves in its fan direction and bends toward `its direction +
tipCurve x up` over 80 % of its length (`bentPath`; the 5th `Split` value, default 1: up, the whorl on its tip on top -
level sheets; negative: out sideways - an elm's arching tips). The continuing and final branches (`bentPath`) take the
trunk shape's `Wobble` as a random walk after their first bend half (snaking limbs; the first ring still meets the
parent square).
**FILL** (the 6th `Split` value 1): the stem stands straight (a third of the shape's curve and wobble, no elbows) and
its end is the centre of a half sphere of `Crown Radius`; the root cone is 80 degrees about up. Each split divides its
cone among its children - child half-angle x 0.62 (2) / 0.55 (3+), its centre offset to the parent cone's edge at even
azimuths (a golden angle per node, +-0.15 rad) - with 3+ children the first keeps the CENTRE direction and the others
share the ring (all on the ring left the dome's middle empty: a bowl) - at least 8 degrees above level - and each child grows toward the point
at its cone centre x `Crown Radius` x (rounds done / depth)^0.8 (`bentPath` from its parent's direction, the shape's
wobble): the final tips land on the dome, every round fills its own share of it, little randomness. A fill branch
tapers to 0.7 of its start (not the fan's 0.45: with the 1 / sqrt(count) per split three rounds left ~5 mm tips). `angle` is
unused; `tipCurve` x 0.5 tilts the last round. An optional 8th value adds SHOOTS (0..8, default 0): per inner split branch - not
the first round's, the innermost - that many short side branches (35..85 % along it, golden-angle sides, out and up,
0.35 x its length, half its thickness), each ending in a `Crown Top` whorl: leaves inside the crown, not only on the
dome (the elm: 2). An optional 7th value s raises the dome's MIDDLE: target y x
`1 + (s - 1) x cone.y^2` - the up-pointing cones rise, the sides stay (default 1: a half sphere; a uniform stretch only
lifted the whole canopy). **SPLIT LIMBS**
(`Crown SplitLimbs count angle length`): per stem `count` side limbs from the crown's start to 0.9 of the stem, evenly
(jittered), at azimuths in the stem's sector (or a golden spiral), `angle` from up (+-8), `length` x the stem height x the crown
SHAPE's envelope at the limb's height in the whole tree (Cone: long low, short high),
0.6 x the stem's radius there tapering to 0.6 of that, ending open into one round fewer of the same splits (at least
one; the first round half the limb's length). Bark and the trunk's wind (bone 0). The `Crown Top` whorl sits at every FINAL tip (instead
of the stem's end); `Leader` is ignored. Off (count < 2) a stem draws its randomness as before.

**MULTI-STEM TRUNKS** (`Trunk Stems`, `generateTrunk`): a trunk piece of n > 1 stems spaces them evenly around the
base (a random start azimuth, +-0.4 rad each), each `Radius` thick, leaning out, the first the full height and the
others 0.7..0.95 of it (with a `Fork` every stem 0.7..0.95: one full-height stem stood out of the crown). `Crown Slots` are split between the stems by their LENGTH (each its own crown, `Tiers` within it; an even split left
the tallest stem's layers sparse - lopsided cedars). Each stem's limbs fill a SECTOR centred on its outward direction
(a golden-ratio fill): 1.2 x its share of the circle with a `Fork`, 1.3 x its share (at most 0.65 of the circle) without one - a few
limbs past the side of its stem into the clump's middle (a full circle per stem, and 1.6 x the share, crossed the
limbs of a birch clump there; exactly the share split a two-stem clump in the middle). Without a fork the sector opens to the full circle over
the upper half of the stem's crown (smoothstep 0.5..1 of the slot height): the stems lean apart there and the limbs
are small. A slot
facing into the clump is also shortened to 0.5 of its length (facing out: 1) - the crowns share the middle. Each stem gets
its own `Leader`. All stems are bone 0 (the trunk bend); the piece's `baseRadius` is `Radius + StemSpread`. One stem
draws its randomness in the original order: a one-stem species is unchanged. Undergrowth: Oak and Pine 4 bushes
out to 8 m; Acacia 6 out to 12 m (its trees stand far apart).

**The open floor is filled by WIDTH, not count** ("dense" = less open space, not more leaves): a bush's cover grows
with its radius squared, so one wide, low ground-cover bush per climate - `Weight 2`, half the climate's bushes -
fills what three round ones would, for one piece (one slot in the set, one cull entry, one far splat):
* `Bramble` (Temperate), `DwarfBirch` (Boreal), `Brush` (Savanna): crown `Radius` 2.6, `Scale` 0.8-1.2, 14-16
  near-flat slots from the ground, `Billboard Distance` 30. LEAVES ON EVERY LEVEL (`Levels 3`) and THIN stems:
  `Module Radius` 0.005, trunk `Radius` 0.02. **`Module Radius` is a fraction of the module LENGTH** - with
  `Length 0` that is the crown radius, so a "small-looking" 0.022 x 2.6 m made ~11 cm stems: branchy, not leafy.

The round bushes, two per climate:
* Temperate: `Shrub` (broadleaf, round, sparse cluster cards) and `Thicket` (denser and wide, flat slot angles).
* Boreal: `Juniper` (a low SPREADING conifer, needle-spray cards, uncrossed - crossed read too dense) and `Bilberry`
  (knee-high, many thin upright stems with small dense leaf clusters).
* Savanna: `ThornScrub` (low, wide, umbrella crown, zig-zag grey stems, pinnate pads) and `Bushwillow` (an upright
  vase of stems, dry olive-yellow broadleaf clusters).
* BirchWood (the birches' own climate name, so they take no Juniper - a needle bush read wrong under birches): `Hazel`
  (a clump of straight slender stems fanning out, broad round leaves, ~2-3 m) and `Raspberry` (low canes arching over
  at the tips, broad leaves), both `Weight 2`, plus `Bilberry` and `DwarfBirch` (`Climate Boreal BirchWood`).

A bush is a tree species with a short trunk, slots from the ground (`Crown Start 0`), a large `BranchRadius`
(the thin trunk would otherwise cap the module scale) and a short `Billboard Distance`.

### Piece mesh LODs

Every piece has `TREE_PIECE_LODS` (4) mesh level slots, all from ONE skeleton: generation first records a
`PiecePlan` (branch paths, stubs, leaf placements — every random decision), then `meshPiece` meshes each level from
it (`PIECE_LODS` table in TreeGenerator.cpp):

| Level | Rings / sides | Branch levels | Leaves (every Nth, scaled √N: same area) | Stubs | Error (× length × ErrorScale) |
|---|---|---|---|---|---|
| 0 | 100 % / 100 % | all | 1 | yes | 0 |
| 1 | 60 % / 60 % | all | 2 | yes | 0.001 |
| 2 | 40 % / 45 % | deepest dropped (never below level 1) | 4 | no | 0.0025 |
| 3 | 25 % / 34 % | two deepest dropped (never below level 1) | 8 | no | 0.005 |

**The GPU uses LEVEL 0 ONLY: no `createMeshLodChain` for tree pieces** (`uploadLodChain` uploads level 0 and leaves
the chain empty). The trees switch through their own tiers (mid tier, billboard, far volume); mesh LOD steps
underneath popped visibly (fewer, larger leaf cards per level, no crossfade). The generator meshes level 0 only
(`MESHED_LODS` = 1 in TreeGenerator.cpp; the table stays for a return of mesh LODs); `lodError` / `Lod ErrorScale`
are unused, and RendererVK allocates no hysteresis slots for tree records.

### Far representations (`Trees/Far mode`)

The SHARED pieces — modules AND trunks, and the baked whole-tree variants — get a billboard each (a trunk's +Y is
world up, so its billboard's "top" card stands vertical: two crossed vertical trunk cards), crossfaded in per placed
piece beyond its distance × `Trees/Far distance scale` (default 4; `Trees/Force far` shows them all). Two modes,
`Billboards` and `None`; changing the mode reloads. The bakes live in `TreeImpostor.cpp`: a small software rasterizer
(2×2 supersampled, back faces culled, leaves alpha-tested). Cache files `<species>_<kind>billboard<i>v<views>_<hash>.png`
(+ `_normal`; kind "" = module, `trunk`, `tree` = baked variant; stale files of the same prefix are removed).
`treeBakeHash` names them by the bake's INPUTS: the species file's text, the piece's set and index, `BAKE_VERSION`
and the settings - **never by the geometry**: its floats differ in their last bits between Debug and the optimized
builds (`/fp:fast`), which renamed (and re-baked) the cache at every config switch. **A generator change that moves
geometry must bump `BAKE_VERSION`** (TreeImpostor.cpp), or press `Trees/Regenerate textures`.

* **Billboards** (default): two crossed cards along the module axis from its LOD-0 box — a vertical card
  (normal +X) and a horizontal card (normal +Z = world-up after the composite), their views stacked as strips
  of one square texture (`bakeBillboards`, `billboardMesh` in TreeImpostor.cpp). `Trees/Billboard views`
  (reloads): **2** (default) — a strip per card (side, top), the back face showing the front view through the
  card; **4** — each FACE its own strip, top to bottom: side from +X, side from −X, top, bottom. A back view
  keeps the card's right / up and negates only the view direction: stored as seen THROUGH the card, it reads
  the right way round on the back face, and its tangent-space normals match the back face's TBN (same tangent /
  bitangent, the handedness flips) — no shader change. Both card PLANES pass through the branch axis (module
  x = 0 / z = 0), not the box centre, so the root lands at the trunk. **Whole trees** (the baked variants, whose +Y
  is up, so both cards above stand vertical) get a THIRD, horizontal card for the top-down view
  (`billboardHorizontalView`): across the axis at `TopCardHeight`, seen from above, u (+Z) spanning at least the
  tree's height so the lit FS's crown radius matches the vertical cards'. The material carries
  `MATERIAL_FLAG_BILLBOARD_TOP_CARD`; the lit FS finds the card by its up-facing normal (RendererVK). The
  edge-on fade hands over between the vertical cards and it.
* **Strip layout** (`billboardLayout`): 3 cards share the texture (3 strips with 2 views, 6 with 4), each strip's
  height rounded DOWN to a multiple of the largest power of two p that leaves ≥ 4 px per strip at mip log2(p) — so
  every strip boundary lands on a texel boundary at every mip the material gets and no mip mixes two cards. 512 / 3:
  p = 32, 160 rows a strip, 32 spare rows at the bottom. 2 cards: 256 rows. Mips stop at ~4 px per strip.
* **THE CROWN FIELD** (`buildCrownField`, TreeImpostor.cpp): the piece's LOD-0 leaves (its bark when it has none)
  splatted by area into a grid of cubic cells (`CROWN_RES` = 32 along the longest side), Gaussian-blurred (sigma 2.5
  cells, ~8 % of the longest side) and normalized to a peak of 1 - a soft hull of whatever shape the leaves make.
  Per texel, at its leaf's 3D point (the card point + the baked depth): the **hull normal** = the field's outward
  gradient (the box centre where the field is flat), and the **interior** = the optical depth out along that normal /
  the field's CORE depth (from its peak out to the side, the mean of 4 horizontal directions), clamped to 0..1 -
  linear, so the lit FS's `Foliage interior depth start / end` remap it, and per-tree normalized, so a narrow pine and
  a wide oak both reach ~1 at their core. **VOLUME normals**: each texel's normal is bent by `Billboard NormalBend`
  toward the hull normal (not "out of the box centre": on a tall tree that points up at the top and down at the
  bottom) and keeps its sign, in each card face's tangent space. **The normal image is the CROWN layout**: RGB =
  that normal, **A = the interior** (RendererVK: remapped, x `Trees/Foliage interior shadow`); the mips are the
  bark's `buildNormalMips` (RGB renormalized, A averaged).
* The billboard materials carry `MATERIAL_FLAG_BILLBOARD`, so the lit FS does not reject the sun shadow by the flat
  card normal — without it a whole card went dark whenever the sun was behind it. They and the leaf cluster
  material also carry `MATERIAL_FLAG_LEAF`: the sun shines through them (RendererVK leaf transmission on `LitTree` /
  `LitFoliage`, `Trees/Foliage transmission*` tweaks). The lit FS also blends their normals toward a view-ray CROWN
  normal (an ellipsoid of the card's proportions; `Trees/Foliage crown normal`), so the two cards stop shading
  differently at their crossing axis. This needs the card's +u axis to run from the instance origin
  (`billboardViews`: right = piece +Y). A billboard material must draw on `LitFoliage`; **the cards cast their own
  alpha-tested shadows**, so the mesh nodes stop drawing entirely past the band.
* **Crossfade** (billboards only): a band `FadeWidth` wide, centred on the billboard distance × `Far distance
  scale`. Inside it (centre distance ± the piece radius) the piece draws its mesh as `barkFade` / `leavesFade`
  — the same meshes on `LitTree` with species materials DERIVED with a distance fade-OUT
  (`Renderer::deriveMaterial`, shared textures) — plus the billboard, whose material fades IN over the same
  band. The tree FSs' dither (tree_fade.inc.glsl, RendererVK `MATERIAL_FLAG_DISTANCE_FADE`) gives each pixel to
  exactly one of them; TAA blends the per-frame dither. Outside the band only one side draws, the mesh on its
  normal (early-depth) materials. `applyFadeBands` writes the bands into the material flags (at reload and when the
  distance scale changes — no reload). Not dithered in the shadow pass: inside the band both cast. `Force far`
  keeps the hard switch. Turning the cards about the branch axis toward the camera was tried and dropped (the look).
* **RT (GI, RT shadows, RTAO, reflections) sees only the WHOLE-TREE billboards** of TREE species: every other tree
  mesh is created without a BLAS (`createMesh(..., raytraced = false)`) - bark / leaves / trunk / branches, the card
  meshes, the module / trunk billboards (the library rows) - and BUSHES never get one. Without billboards (Far mode
  None) the tree species' bark + leaves are raytraced instead.
* **Branch cards — the MID tier** (GPU path, baked variants; `Trees/Branch card distance`, default 0.4 × each
  species' billboard distance, 0 = off; respawns): every baked variant keeps its composite (`TreePiece::placements`)
  and its bark SPLIT in two (`trunkBark` - the trunk alone - and `branchBark` - the modules'; `bark` stays whole for
  the bakes and the CPU path). `buildBillboards` stacks the module billboards into ONE CARD ATLAS per species (module
  m in rows [m, m + 1) x size; each mip the stack of the modules' own coverage-preserved levels) and merges per
  variant ONE CARD MESH: every module placement's cards in the variant's space, v into the module's block. Between
  the full mesh and the whole-tree billboard the tree draws the TRUNK (shared by both - it never fades at the mid
  band) plus the CARD mesh: over the mid band (`FadeWidth` wide) the branch bark (`branchMidFadeMaterial`) and the
  leaves (`leafMidFadeMaterial`) fade out while the cards fade in (`cardInMaterial`); over the far band the trunk
  (`barkFadeMaterial`) and the cards (`cardOutMaterial`) fade out while the whole billboard fades in (derived
  materials, bands in `applyFadeBands`). The cards draw on LitFoliage with the whole-tree billboards' shading and
  "Foliage ..." tweaks, but with `MATERIAL_FLAG_NO_EDGE_FADE`: no edge-on fade. Their crown frame is still per
  MODULE: `billboardMesh(..., axisInZ)` puts each card's axis (its module's +Y line, as a texture v) into
  texCoords.z = 3 + v (`TREE_CARD_AXIS_CODE`), which rides the tangent's w magnitude to the lit FS. On top of it the
  card's WIND, + `TREE_CARD_WIND_STEP` (4) x (weight | phase << 4): the vertex's weight along its module's +Y (0 at
  the root, 15 at the cards' top end) and the placement's `windPhase` (`TreePiecePlacement`, set by
  `bakeTreeVariant`: the module mesh's phase) - the cards bend per module like the mesh (RendererVK "Tree wind"). MAIN pass only:
  shadows, GI and RT keep the whole billboard. 4 fixed record slots per tree, no pool (RendererVK "BAKED TREE
  RECORDS"). The CPU path (GPU expansion off) has no mid tier.
* **None**: the meshes only.

### Texture files (`Assets/Local/Trees/Textures`)

Generated output under `Assets/Local`, so NOT in git: a fresh checkout generates them on the first load.
The species textures are generated ONCE and saved as PNG (`File:ImageIO`): `<species>_bark.png`,
`<species>_bark_normal.png`, `<species>_leaves.png` (cluster species only), plus the billboard bakes. Later loads
read them back and only rebuild the mip chains (bark box-filtered, leaves coverage-preserving). **A file that exists
wins — changing `Bark` / `Color` / cluster parameters does NOT regenerate it**: press `Trees/Regenerate textures`
(overwrites all) or delete the file. The files are also where authored replacements go: square power of
two, RGBA8; the leaf atlas keeps the 2×2 cell layout (stem at the TOP edge of each cell, v = 0); the normal
map is tangent space with x along u (around) and y along v (along the branch, DOWN the image). The rocks' DEAD WOOD
reads the bark pair too ([Rocks.md](Rocks.md)).

**GPU formats** (`Trees/Compress textures`, default on, reloads): the PNGs stay the RGBA8 cache; at load the
finished mip chains (coverage-preserving alpha, AlbedoScale applied) are BC-compressed in memory (File
`TextureConvert::compressBlockRows`, stb_dxt, every level's block rows a `parallelFor` task, "Tree texture BC"):

| Texture | Format |
|---|---|
| bark albedo | BC1 sRGB (opaque) |
| bark normal | BC5 (XY; MATERIAL_FLAG_BC5_NORMAL rebuilds Z) |
| leaf cluster atlas | BC3 sRGB (alpha-tested) |
| billboard + card-atlas albedo | BC3 sRGB |
| billboard + card-atlas normal | BC3 linear - RGB the full normal WITH its sign, A the baked interior: no two-channel format holds both, and there is no BC7 encoder |

4x less VRAM (BC1: 8x). Off = the RGBA8 chains (the A/B for compression artefacts). The module / trunk
billboards are uploaded only with `Show piece library` (default on; they draw only there; that tweak reloads too).

### Bark texture

PROCEDURAL per species until authored textures exist (`TreeBarkTexture.cpp`, 1024²): tiles in both directions,
matching the bark UVs (u once around a branch, v along it per base circumference — so the pattern scales
with each branch). **Furrows, not cells** (a Worley plate pattern read as scales): two families of fissure
lines along the branch, each meandering by its own tileable fbm, cross and merge into braided ridges. **All
noise is tileable GRADIENT noise** — smoothstep value noise has zero slope on every lattice line, which the
normal map shows as evenly spaced horizontal bands. Every line fades out where its own per-column noise is high
(`Breakup` → segments with gaps) and varies in width along its length; a third, 3× denser family of FINE cracks
(mostly gaps, shallow) splits the ridges; a fourth, 8× denser family of STRIATIONS (thin, very shallow) runs along
the ridges; rare thin horizontal breaks. **Low contrast in the cracks** (albedo 75 % of the bark colour, fine cracks
85 %; fissure floor at 35 % of the ridge height): dark or deep outlines read as cartoon ink. Rounded ridge crowns
with V fissures, ridge-scale height/tint from SMOOTH noise (a per-segment hash makes a brightness grid), fine grain,
along-the-branch fibres and lichen on the ridges. BIRCH MARKS (`Lenticels`, `Patches`, in `Color Mark`): dark dashes
ACROSS the branch (`lenticel`: one per lattice cell by chance, an ellipse, slightly sunk) and dark patches (a
thresholded fbm, 2 cells around x 4 along, the grain deepened in them). SECTIONS (`Sections`, in `Color Section`,
under the marks): soft areas of another tint (a thresholded fbm, 3 around x 2 along: longer along the branch) - a
birch's brown older bark. sRGB albedo plus a LINEAR RGB tangent-space normal map from the
same height field (x along u, y along v — the tube's tangent / bitangent), each with a CPU box-filtered mip chain
(normals renormalized). Uploaded through `Renderer::createTextureMaterial(..., &normalMips)`, freed with the species.

### Leaf types

* **`Single`** — one double-sided diamond per leaf, the species' solid leaf colour, `LitOpaque`.
* **`Cluster`** — alpha-tested CARDS (`LitTree`), each showing a twig with `ClusterLeaves` leaves. `Size` /
  `Aspect` are then the card's, `PerBranch` counts cards, and a card's stem starts at the branch centre (the
  texture's twig grows out of the wood). `Cross true` adds a second card at 90° about the stem axis.
  The texture is PROCEDURAL until authored textures exist (`TreeLeafTexture.cpp`): per species a 1024² 2×2
  atlas of cluster variants (stem at v = 0, each card picks a cell), sRGB colours from `Color Leaf` / `Bark`,
  and its OWN mip chain — every level's alpha is rescaled (binary search) so the fraction of texels passing
  `TREE_LEAF_ALPHA_CUTOFF` (0.5) matches level 0, otherwise the crown thins with distance. Uploaded through
  `Renderer::createTextureMaterial`, freed with the species (`TreeSystem::clearAll`). Every leaf / needle / shoot /
  pinna is shortened until it fits inside its cell (the cell clips the drawing).
  **`Style Needles`** (conifers, `drawNeedleCluster`): each cell is a conifer SPRAY instead of a leafy twig - `Shoots`
  needle branches fanned from the stem (the middle one along v, the others over up to ±18° and a little shorter;
  one card then reads as a whole spray, so the tree needs fewer branches and cards), each a main shoot plus
  `ClusterLeaves` side shoots (alternating, short, 24-38° off it - wider reads as a flat fan), all densely set with
  thin tapered needles on both sides and a forward tuft at the tip. `ClusterLeafSize` is the needle length.
  **`Style Mat`** (foliage pads, `drawNeedleMat`): a lobed PATCH on a twig SKELETON - a main twig in from the cell's
  base edge (v = 0) to beyond its middle, 4-6 side twigs off it (alternating, 35-80 degrees), and `ClusterLeaves` short
  needle shoots (0.1..0.2 of the cell) along all of them (picked by twig length, 30-75 degrees off it); the twigs drawn
  over the shoots, a third of the shoots again over the twigs. `ClusterLeafSize` is the needle length. The outline
  follows the twigs (shoots scattered over a disc read as round blobs), and the twig gives each blob its branch.
  **`Style Pinnate`** (acacia, `drawPinnateCluster`): `Shoots` BIPINNATE compound leaves fanned from a short woody
  twig (±28°), each a thin greenish rachis with `ClusterLeaves` pairs of LONG pinnae (0.55 × the rachis, forward,
  shorter toward the tip; the first pair close to the twig - a long bare stem reads as a stalk), every pinna a dense
  COMB of narrow oblong leaflets (`ClusterLeafSize` long). Short pinnae nearly at right angles with sparse wide
  leaflets read as scattered specks. **Each of the 4 cells has its own CHARACTER** (`PinnateVariant`: lean, rachis
  curve and length, pinna count / angle / length, leaflet size, alternate pinnae, tint) - with jitter only the four
  read as copies.

### Baked tree variants (what the grove places)

The runtime does not composite pieces per tree: at load every species bakes `Bake Variants` (4) whole trees
(`bakeTreeVariant`: the composite of a seed, all its pieces' bark merged into one mesh and all their leaves
into another, PER LOD LEVEL, at scale 1; each level's error = the largest piece error × its placement scale).
A variant is just one more "piece" (`Species::variants` / `variantMeshes`): a billboard (cache kind `tree`), the
crossfade — all the piece machinery applies unchanged. The grove and the world place one variant per tree with a
seeded variant, scale (`Scale`) and yaw. The pieces still exist (the bake input, the piece library rows).

**The WIND payload** (RendererVK "Tree wind", `tree_wind.inc.glsl` for the bit layout): `meshPiece` writes every bark
and leaf vertex's `texCoords.z = 1 + payload / 2^22` (`windPayload`): the module weight along the module's root bone
(a sub-branch's vertices take it at their bone's pivot), the level-1 sub-branch weight along its bone, the bone's phase,
a leaf card's TIP flag (corners 2 / 3; a diamond's all but the stem) - a card's weights from its STEM, so all four
corners share one phase. Trunk pieces: the wind bit only (the trunk bend). `bakeTreeVariant` ORs each module
placement's phase (3 bits) into its vertices and keeps it in the placement (`windPhase`: the branch cards' phase). No
vertex grows: the magnitude of the tangent's w was free. The species' `Wind` scales ride its piece TYPES
(`TreeInstanceType::windTrunk / windBranch`, RendererVK "Tree wind"): the cedar (large, heavy limbs in the trunk
piece) `Trunk 0.4`, `Branch 0.7`.

### GPU expansion

With `Trees/GPU expansion` (default on), `spawnPreview` builds ONE RendererVK tree instance set
(`createTreeInstanceSet`): a piece TYPE per library piece (its bark, leaves, the derived fade-out materials and the
billboard, plus the band), then every placed piece (transform, far centre + radius, type). The set is uploaded once to
device-local memory, and the culls make the per-piece decision and build the records themselves, and the TLAS writer
their RT instances (see RendererVK "BAKED TREE RECORDS"). **The pieces live in the TERRAIN CHUNKS:** `spawnPreview`
sorts them by the terrain chunk under their base (`Globals::terrain.chunkSize()`; a chunk-size change respawns), one
set chunk per terrain chunk, and hooks the set into the terrain (`TerrainStreamer::setVegetation`,
[TerrainStreamer.md](TerrainStreamer.md)): the terrain's render walk lists the chunks it draws with their passes, and
its sink calls `renderTreeInstanceSet` with that list - so the culls only see the plants of the visible chunks (all
passes) and of the shadow / GI sphere's chunks (shadow + GI). **The sink runs on the walk's worker: it reads the band
settings through atomics.** `update` binds the set (`bindTreeInstanceSet`, main) and submits EVERY chunk itself when
the walk does not carry it (terrain off, or the respawn frame - the new hooks start with the next walk). Unhooking
(`destroyTreeSet`) joins the walk.

Every baked variant also carries its FAR-TREE VOLUME grid (`bakeTreeDensity`, `TREE_DENSITY_RES` = 32³ extinction
over its billboard box, from the LOD-0 LEAF triangles only; cluster leaves count half opaque. No bark: a trunk's whole
area in one thin voxel column draws dashed vertical streaks through the far blobs) and the species leaf colour — the
leaf texture's alpha-weighted mean, DECODED FROM sRGB (`meanLeafAlbedo`; the albedo textures upload as sRGB, so the
raw `leafColor` taken as linear reads light yellow-green) — handed to the set as the type's density / albedo
(RendererVK "Far-tree volume"). With billboards, the BILLBOARD is the piece's only shadow / GI / RT representation at
every distance: the mesh draws in the MAIN pass only (both paths). Off, it keeps the CPU path ("The preview": one
`RenderNode` per piece representation, pushed per frame). The set is destroyed on respawn / reload / disable.

### World records (`TreeWorld`, "Trees/World" tweaks)

Every tree of the terrain ring as a **4-byte record per terrain chunk** (`TreeRecord`: x, z 12 bits each, chunk-local
on a `TREE_RECORD_STEPS` = 4096 lattice; the type 8 bits = its index in the NAME-SORTED `.tree` list, then the
`.rock` list). No height (the terrain's), no variant / scale / yaw: `treeRecordSeed(seed, chunk, record)` hashes the
chunk and the QUANTIZED position, never the record's index, so a change elsewhere in the chunk leaves a tree as it
was. **Mirrored in `Assets/Shaders/Trees/tree_record.inc.glsl` - keep in step.** A member of TreeSystem, updated
before its `Enabled` check: it runs with the preview off.

* **Ring:** the terrain's (`ringRadius()`, `chunkSize()`, `generatedBounds()`), one chunk of hysteresis on eviction.
  On a camera chunk change the missing chunks are requested and the queue re-sorted nearest first; Low pump jobs
  (`Gen jobs`; 0, the default = one per job worker - `pumpCap`; they yield at every preemption point, so they only
  fill idle workers) take the front, with lazy staleness as the terrain pumps.
* **THE BOOKKEEPING JOB** (`"TreeWorld update"`, Normal, `m_updateCounter`): merging the pumps' results, the ring
  rescan (a walk of every chunk + the (2R+1)² ring) and the next upload pick run OFF main. main.cpp kicks it
  (`TreeSystem::kickWorldUpdate`) right after `clutter.update` - the LAST reader of the records this frame
  (`cpuRecords`) - and the next `TreeWorld::update` joins it first, **so nothing reads the chunks while it runs**. It
  queues `m_pendingRemoves` (handles of the chunks that left the ring) and `m_pendingUploads` (picked chunk keys).
* **THE POOL JOB** (`"TreeWorld pool"`, Normal, `m_poolCounter`): the next `update` kicks it - the record POOL's
  removes and adds (`applyGpu`), then `updateTreeRecords`. The pool takes one thread at a time between beginFrame and
  `present`, which reads it: **main.cpp joins it before `present` (`TreeSystem::joinWorldPool`), TreeSystem before
  `setTreeRecordTypes`, and `kickUpdate` before the next bookkeeping job.** It writes `Chunk::gpu` ONLY, so TreeSystem
  and the clutter read the records beside it; the bookkeeping job drops the uploaded chunks' ground (and their CPU
  records outside the keep radius) first thing (`finishUploads`). One frame between a pick and its upload.
* **Main keeps** only the rare parts: the config check, a restart (it resets the pool), `loadSpecies` (IO), and the
  job inputs (camera chunk, ring radius, keep radius, upload budget) `update` snapshots.
* **Pure function** (`placeChunk`): one `sampleGrid` at Full detail, 8 m step + halo, then one candidate per cell of
  a `Candidate cell (m)` (5) lattice, jittered over the cell. Per species (`Placement`) its CLIMATE FIT: 1 inside its
  IDEAL box (`Temperature` / `Precipitation` min max, normalized by `temperatureTo01` / `precipTo01`), outside a
  Gaussian of the distance to the box (per axis, sigma `ClimateWidth`), faded to 0 between `Climate fade start` and
  `end` (0.10 / 0.25 of the peak), 0 outside its slope / altitude band. (A box, not an attractor point: a point
  thinned every climate around its peak.)
  Each species' density at the candidate: Density x fit x its cluster fbm x (fit / the BEST fit there) ^
  `Climate sharpness` (4). The candidate exists with probability (the summed densities) x the cell's hectares x
  `Density scale` (so at most one tree per cell: 400 / ha at 5 m); its species is picked by share. **Species that
  share a climate ADD**: a rare species in a dense one's box (Willow in Oak's) takes nothing away from it. (Picking
  by Density x fit let a dense species' tail outnumber a sparse one's core; normalizing by the SUM halved an
  equal-fit species.) **GRASSLANDS** ("Grassland share", 0.25; "Grassland size (m)", 800): a world fbm (3 octaves at
  that scale, its own seed) below a threshold is open land - every species' density x 0 there, x 1 elsewhere over a
  narrow edge (+-0.02 of the noise) - so whatever the climate there are open plains, and the forests between them keep
  their FULL density (lowering the density thinned every forest instead). In the open, "Grassland trees" (t, 0.5)
  keeps the largest of: COPSES (a second fbm at 1/8 of the size above `0.8 - 0.15 t`: small groves at 0.7 of the
  forest's density), the FOREST EDGE thinning out (0.4 t at the edge, to 0 over 0.1 of the noise into the open) and
  LONE TREES (0.05 t everywhere) - the climate still picks their species. The threshold `0.5 + 0.3 x (share - 0.5)`
  assumes the noise spreads about +-0.15: the share is approximate. In `speciesWeights`, so the rocks' FOREST term sees
  the plains too. **SPACING** (`Placement Spacing`): every CANDIDATE (the existence roll and the species pick - a pure
  function of the seed, the chunk and the cell, `candidateAt`) gets a priority (hash 6); a candidate gives way to one
  of higher priority within max(both spacings). Decided on the candidates, not on the surviving trees, so nothing
  chains: a chunk evaluates the neighbours' candidates within `ceil(max spacing / cell)` cells of it exactly as they
  do, and both sides of a border agree. The field and river grids widen their halo to that reach (the same world
  points: no value inside changes). Placeholders: Elm 11, Cedar 9, Acacia 6, Oak / Willow 5, Pine / Birch 3 m; without
  any spacing the chunks place what they did before. **FOREST TYPES** ("Forest type size (m)", 600; "Forest minority", 0.15): one species DOMINATES a
  stand - each species has a large-scale noise (its own seed, 2 octaves at that size); of the species present at a
  point the highest noise wins, the others x the minority factor (fully once 0.06 of the noise behind the winner: a
  soft border between stands), then all rescaled to the same total - which trees grow changes, not how many (birch
  forests among the oaks' and pines' edges, not every species in every wood; 1 = off). Bushes are not stored. No tree or rock where the river influence is above "Terrain/Rivers/
  Vegetation clear" (except rock rules with a River term, below).
  **The existence roll goes FIRST** (exact, the same result): a tree candidate whose roll is above the species'
  summed `Density` x the cell's hectares, or a rock cell whose roll is above its type's bound (every rule's `Density`
  x its largest ground / forest / river / flow multipliers - every fit term is at most 1), skips its field sample and
  fits. The river grid (1 m) covers only what is read: the chunk plus the neighbours' rock reach + 2 m, not the
  terrain halo.
* **A new generation** (everything dropped, every chunk regenerated) on: Enabled / Seed / cell / density / climate /
  grassland / forest type tweaks, `Reload species`, a new sampler, a chunk size or bounds change, a rock change (`setRocks`). The generation
  counter is monotonic, so a late pump result of an old config never merges.
* **GPU:** each merged chunk joins an upload queue; `Upload KB per frame` (1024) of records per frame go to the
  renderer's pool (`Renderer::addTreeRecordChunk`, RendererVK "WORLD TREE RECORDS"; `GPU pool (MB)`, 128, fixed - a
  full pool refuses chunks and warns once; a change drains the GPU and regenerates), each with its GROUND:
  `placeChunk` also writes the chunk's `TREE_RECORD_HEIGHT_RES`² = 17² heights (16 m at 256 m chunks, bilinear in its
  Full 8 m grid, 16-bit above the minimum - TreeRecordPool's encoding), the ground the far volume stands the records'
  trees on (the camera-centred height map's far cascade is too coarse: it buried them on every peak). Every chunk
  uploads, trees or not. An evicted chunk's GPU range is freed (deferred reuse). Disabling frees the pool; a ring
  radius change restarts (the pool's chunk map is sized by it).
* **CPU keep radius (chunks)** (4; at least the world mode's near radius + 1, `requireKeepRadius`): the CPU keeps a
  chunk's records only inside it (world mode's expansion and the clutter's floor map read them). Beyond it they are
  dropped once uploaded; a chunk coming back inside is generated again (same records - nothing re-uploads).
* `Log stats`: chunks (in flight, queued, to upload), trees, per chunk average / max; CPU and GPU MB; per species of
  the CPU-held records.
* Placeholder ideal climates - SEPARATED (overlapping boxes put every species in every temperate wood): Pine -6..3 C /
  600..2200 mm, 90 / ha; Birch 4..9 C / 600..1800 mm, 60 / ha, BirchClump the same, 30 / ha (the cool band between
  Pine and Oak); Oak 10..18 C / 900..2200 mm, 90 / ha; Willow 11..18 C / 1300..2200 mm, 1 / ha (the wettest of Oak's);
  Cedar 9..16 C / 350..850 mm, 10 / ha (Oak's warmth, too dry for it); Elm 9..16 C / 700..1500 mm, 40 / ha (Oak's
  drier, cooler side - Forest types make elm woods among the oaks); Acacia 20..30 C / 300..900 mm, 12 / ha;
  `ClimateWidth` 0.04 (Acacia 0.06).
* **ROCK RECORDS** (`setRocks` - on with "Rocks/Enabled" + "Rocks/World/Enabled", handed over by TreeSystem every
  frame; a change is a new generation): the same 4-byte records in the same chunks. The record TYPES: the `.tree`
  list, then every `.rock` (name-sorted) from `firstRockType()` on - TreeWorld reads the `.rock` `Placement` blocks
  itself. `placeChunk` places the rocks FIRST:
  * **A rock type's lattice is in WORLD space** (`RockSpecies::cell` = 1.5 x its largest `Scale`, at least 6 m;
    anchored at the world origin, one rock per cell at most): a cell's rock is a pure function of (seed, type, cell),
    so a chunk computes the rocks of its NEIGHBOURS exactly as they do. It evaluates every cell that can put a rock
    within `ROCK_REACH` (11 m: half of `ROCK_MAX_SIZE`) of itself; its own rocks become records, the others only keep
    its trees out. For that the sample grid has a halo of `GRID_HALO` = 5 points (40 m: the rock's reach, the talus
    probe and the rugged measure's 2 cells) WHILE ROCK TYPES ARE PLACED (43² samples per 256 m chunk; a world without
    rocks keeps the 1-point halo, 35²) - plus the valley measure's coarse grid, 21² more. Chunk-generation cost
    matters little; the per-frame cost is what counts - all of this is in the pump jobs.
  * **A type has any number of RULES** (`RockSpecies::rules`: its `Placement` blocks) and **their densities ADD** -
    a rule for everywhere plus one for a climate the type is common in, each with its own ground (`Slab.rock`'s
    second block: common in the savanna). A rule's fit at the rock's (quantized) record position: its optional
    climate box (as a tree's), its `Altitude` band, x its `Slope` band (the slope UNDER the rock; soft edges with four
    values, `bandWeight`), x `mix(1, crag, Crag)` (the terrain shader's rock coverage - steep, or far above the macro
    altitude - at the terrain's DEFAULT texture tweaks: `CRAG_*`, a deliberate approximation), x `mix(1, talus,
    Talus)` (gentle ground with a steep slope `TALUS_PROBE` = 6 m uphill of it), x THE GROUND AROUND IT (below), x
    its FOREST and RIVER / FLOW terms, x its cluster fbm (a noise per rule). The rock exists with probability (the
    rules' summed Density x fit) x the cell's hectares x "Rocks/World/Density scale". The record does not keep the
    rule: what a rule cannot set (`Scale`, `Sink`, `Align`, the shape) is the type's.
  * **THE GROUND AROUND A ROCK** (per rule). The slope under the rock cannot tell a cliff's foot, a crest or a valley
    floor from the plains. ground = `mix(Plains, mix(Rugged high, Rugged low, lowEnd), ruggedness)`, then - when the
    rule has a `Valley` - `mix(ground, Valley, valley)`: **IN A VALLEY THE MULTIPLIER IS `Valley`**, whatever the
    others say (an override, not a product: Plains 0.3 x a product would need a Valley of 20 for a flat valley floor,
    and that x Rugged at a valley's cliff). Three measures, each only when a rule uses it:
    * `ruggedness` = smoothstep("Rocks/World/Rugged slope start", "full", the STEEPEST grid cell within
      `RUGGED_CELLS` = 2 cells of the rock) - 16-24 m. Each grid cell's slope, a separable square max
      (`windowRange`), read between the cell centres (`gridAt`).
    * `lowEnd` = smoothstep(0.25, 0.75, how far DOWN the rock sits between the highest and the lowest grid point
      within the same 2 points): ~1 at a cliff's bottom, ~0 on its top, 0.5 on an even hillside.
    * `valley` = smoothstep(`VALLEY_LOW_START` 0.5, `VALLEY_LOW_FULL` 0.9, the same over ~160 m) x
      smoothstep("Rocks/World/Valley relief start (m)", "full", that height range): LOW ground with higher ground
      within ~160 m - a valley's floor (flat ones too, up to ~300 m wide), the foot of a mountain. From its own COARSE
      grid (`VALLEY_STEP` 32 m, the lowest / highest point within `VALLEY_CELLS` = 5, a halo of 6: 21² Full samples;
      on the chunks' common lattice, so the neighbours agree). The sampler's macro `altitude` cannot serve: it is the
      coarse stage's 7.68 km / px surface.
    `Plains 0` = never on flat ground (the Block). Control is PER TYPE: a world rule by rock size (large rocks off
    the plains) was tried and dropped.
  * **FOREST** (per rule, `Forest open forest`, for dead wood): the density x mix(open, forest, the trees' own
    expected density there / `ROCK_FOREST_FULL` (60 per ha), clamped) - `speciesWeights`, the tree pass's own sum
    (every species' climate fit, faded, sharpened by the best fit, x its density and cluster patches, x "Trees/World/
    Density scale"), evaluated at the rock. The rocks are placed before the trees, so the measure is what the trees
    WILL be there, not the trees placed. `Forest 0 1`: only in forests, never in their clearings.
  * "Rocks/Reload types" makes TreeWorld read the Placement blocks again too (`RockSystem::typesRevision` through
    `setRocks`); "Trees/World/Reload species" does it WITHOUT regenerating the rock meshes - the fast way to iterate
    a Placement block.
  * **Rocks do not give way to each other** (boulders lie against boulders; a rejection order would chain across
    chunk borders). **The trees give way to every rock**: a tree candidate inside its FOOTPRINT is dropped - a
    CAPSULE along the record's yaw (hash 104, rock-local X turned to (cos, -sin) as the expansion turns it), its half
    length and radius `rockFootprint(type)` x `rockRecordScale(seed, Scale)`: a round rock's has no length and a
    radius of half its size (tree out inside 0.9 x that + 0.5 m); a lying trunk's runs along the log with its
    thickness (+ 0.9 m: a disc over a long log cleared the forest around it); a standing one's covers its base and
    roots. The clutter's floor map uses the same capsules (`FloorType::footprint`).
  * With rocks off the tree records are unchanged (the same hashes, the same field interpolation).
  * The far volume takes the rock records too: see "World mode", ROCKS IN THE FAR VOLUME.

### World mode (TreeSystem)

`Trees/Enabled` + `Trees/World/Enabled` + `GPU expansion` (+ terrain): the trees are TreeWorld's records, not the
preview grove. Switching it, or a TreeWorld restart (its `generation()`), respawns.

* **One DYNAMIC set** (RendererVK `createDynamicTreeInstanceSet`: every piece type, a fixed `Trees/World/Set
  capacity (pieces)` of slots, 600 k; no growth, a full set refuses chunks and warns once), hooked into the terrain
  like the preview (`hookTerrain`; chunk index capacity (2 x near + 3)² x 2).
* **`Trees/World/Near radius (chunks)`** (3, Chebyshev): every chunk inside it that TreeWorld holds CPU records for
  (TreeWorld's keep radius is raised to near + 1: `requireKeepRadius`) is EXPANDED on a Low job, nearest ring first,
  at most 4 in flight: per record its tree (`treeRecordSeed`: the preview's variant / scale / yaw rules, `Size
  variation`) on the ground of ONE Full `sampleGrid` at 2 m (the terrain's LOD 0 spacing) over the chunk plus the
  bushes' reach, plus its BUSHES (its species' `Undergrowth`, else `Bushes per tree` out to 0.75 x `Spacing`; the
  bush species of its climate by `Weight` - the preview's rule; `ExpandSpecies::bushesPerTree` / `bushRadius`,
  `bushUndergrowth`), none in WATER (at the grid's nearest point: under a river's / lake's surface or the sea, or a
  river bed past "Terrain/Rivers/Vegetation clear"; the far volume's GPU mirror has no water data and keeps them).
  **The job reads only an immutable `ExpandContext`** (types, scales, far centre / radius per variant, the record-type
  -> species map by name, the sampler) and a copy of the records.
* `Expand per frame` (2) finished chunks join the set per frame (`addTreeInstanceChunk`), the coordinate is mapped
  (`m_vegChunkOf`) and re-stamped in the terrain (`restampVegetation`). A chunk past near + 1 is removed
  (`removeTreeInstanceChunk`, deferred reuse) and re-stamped; a pending one's result is dropped.
* A respawn joins the jobs (`stopExpansion`: the generation bumps, the results are cleared).
* **THE WORLD'S ROCKS**: `spawnWorld` appends RockSystem's loaded types (`Globals::rocks.worldTypes()`) to the set - a
  GPU type per variant: ONE mesh, the LOD chain's level 0 in the `bark` slot on `LitRock`, no billboard (RendererVK:
  the culls pick its level, no wind) - and maps TreeWorld's rock record types to them by name. `expandChunk` turns a
  rock record into ONE piece (`placeRock`): variant and yaw from the seed as a tree's, the scale from
  `rockRecordScale`, laid ON THE GROUND by `RockSystem::groundTransform` (Rocks, "A rock on the ground"
  ([Rocks.md](Rocks.md)): it follows the ground's normal by `Align`). The set's types are fixed, so RockSystem's
  `worldGeneration()` (its meshes arrived, changed or went - it loads in the background) respawns the set; and
  **before RockSystem frees its meshes it calls TreeSystem's mesh-user hook (`setMeshUser`), which destroys the set.**
  **ROCKS IN THE FAR VOLUME**: rock meshes hand over to the far-tree volume at its start as a tree's billboard
  (RendererVK tree_cull.inc.glsl: a type without a billboard drops its mesh past `Far start` + `Far overlap` in the
  main pass; it keeps casting its sun shadow). A rock GPU type carries its variant's OCCUPANCY grid
  (`RockVariant::density`: `ROCK_DENSITY_RES` = 16³ over its rock-local box, the field soft over one voxel; generated
  with the meshes) as the type's `density`, with `solid = true` and the SINK baked into the box (`densityMin/Max` -
  Sink x height; the volume stands a piece on the ground; the lean is left out). `spawnWorld` hands the renderer a
  `TreeRecordTypeGpu` per rock record type too (albedo.w = 0: a SOLID): its variants as the set's rock types (max
  `TREE_RECORD_MAX_VARIANTS` = 8), its Scale range with no size variation (= `rockRecordScale`), per variant its
  occupied volume at scale 1, the profile over the sunk box. **Keep `placeRock`'s variant / scale / yaw hashes and
  the volume's record expansion in step** (102 / 103 / 104, as a tree's). RendererVK "Far-tree volume", ROCKS, has
  the volume side.
* **The far-tree volume** takes EVERY tree from the RECORDS (RendererVK "WORLD TREE RECORDS"), never from the set:
  within `Trees/Far record detail (m)` the GPU expands each record exactly as `expandChunk` (variant, scale, yaw, its
  bushes - the same hashes; the terrain map's ground) and splats it in detail; beyond, its exact mass per column.
  So a chunk entering or leaving the near radius changes nothing in the volume. The spawn hands the renderer the
  record types (`setTreeRecordTypes`: per TreeWorld type its variants as the set's types, Scale range, size
  variation, a tree's bush types in `ExpandSpecies::bushes` order (weights as repeats), its OWN bushes per tree /
  radius, per-variant mass, the height profile); a respawn clears them. **Keep `expandChunk` and
  `tree_volume_splat.cs`'s `splatRecordPlant` / `main` (and `tree_volume_records.cs`'s variant / scale) in step.**
  At most `TREE_RECORD_MAX_VARIANTS` (8) variants per species and `TREE_RECORD_MAX_BUSHES` (8) bush entries per tree
  (a warning past that). Without `Trees/Enabled` (no species meshes, so no density) the records are not in the
  volume. It re-bakes at most every 30 frames while record chunks change.

### The preview

`Trees/Enabled` loads every species (enable / `Reload species` re-reads the files — no hot reload) and spawns a
`Grove size`² grove in front of the camera plus the piece library in rows behind it (`Show piece library`; reloads -
off, the module / trunk billboards are not uploaded). In world mode the world's set replaces the grove. Bark draws with the species' bark texture on `LitOpaque`; leaves on `LitTree`
(cluster cards) or a solid colour on `LitOpaque` (`Single`).

* `Bushes per tree` (default 4; the fraction by chance) scatters the `Kind Bush` species around each grove tree
  (1.5 m .. 0.75 × spacing out, area-uniform, a bush species by `Weight`, the same variant / scale / yaw rules) -
  unless the tree's species has an `Undergrowth` block, which replaces both.
* `Bush shadow distance (m)` (default 100, 0 = no limit; GPU path): bushes farther than this from the shadow
  cascades' centre cast no sun shadow (`TreeInstanceType::shadowDistance`).
* `Grove type`: Mixed (the TREE species alternate) or one species by its `TreeSpecies` name (default Oak) — the names
  are a fixed list in Settings.Trees (`TREE_GROVE_TYPES`; a tweak enum registers before the species load), so a new
  species needs its name added there; a missing one falls back to mixed with a warning.
* Trees sit on a `Spacing (m)` grid (default 11; `Grove size` 1..512, default 1), each offset by a seeded random
  `Position jitter` × spacing (default 0.8; 1 = anywhere in its cell - a small fixed jitter leaves the rows
  visible), and scaled by the species' `Scale` range × `Size variation` (2^±v, log-uniform, default 0.6 =
  ×0.66..1.52).
* With `GPU expansion` the grove is one instance set ("GPU expansion"); off, one `RenderNode` per placed piece mesh,
  pushed every frame.
