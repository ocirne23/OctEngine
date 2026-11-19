# Trees

> Part of the Procedural documentation ([`Docs/Context/Procedural/CONTEXT.md`](CONTEXT.md)) — read that first.

## Trees

"Trees" tweaks. `TreeSystem` loads every `Assets/Trees/*.tree` species, generates its piece library and baked
variants (through the generated-mesh cache - "The generated-mesh cache" in [CONTEXT.md](CONTEXT.md)), and draws
the WORLD: TreeWorld's records ("World mode"). Without TreeWorld or the terrain no tree draws.

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
	Seed n · Scale min max · Kind Tree|Bush (default Tree; a Bush grows around trees, never as one - and, with a
	        Placement block, also ON ITS OWN: "THE BUSHES ON THEIR OWN" in World records)
	Climate <name> (a Bush grows around the trees of the same climate only; a bush may list SEVERAL names -
	        `Climate Boreal BirchWood` - and grows around the trees of any of them; a tree whose climate no bush shares
	        takes every bush)
	Weight n (a Bush, 1..8, default 1: how often its climate picks it - it is listed n times in each tree's bush list,
	        which the CPU and the far volume's GPU mirror both pick from uniformly; a tree's list holds at most
	        `TREE_RECORD_MAX_BUSHES` = 8 entries in the far volume)
	Wood x (a Tree, default 1: x the wood a LUMBERYARD counts for one of its trees, after the size and the game's size
	        exponent - `TreeWorld::surveyHarvest`'s weight. The Cedar 0.35: its massive buttressed trunk read as ~6 trees)
	Undergrowth { Bushes (per tree, the fraction by chance) · Radius (m: out to this from the tree, at least 1.5) }
	Wind { Trunk (x the trunk lean + sway, 0..4, default 1) · Branch (x the branch sway of the meshes) }
	        (a Tree; absent = "Trees/Bushes per tree" out to 8.25 m; the far volume reads it per record type)
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
	        half width / its length, 0.1..0.6, default 0.24 narrow; 0.45 oval) · Cup (Leaves: 0..1, default 0.5 - the
	        leaf NORMAL MAP's smooth CONVEX dome: the sides falling away from the midrib, the tip curled down, + a random tilt
	        per leaf; "Trees/Leaf normal strength" scales it in the shader. Sides curving UP made a saddle with the tip curl -
	        lit on the wrong half - and a crease at the midrib read as a bright ridge)
	        · Veins (Leaves: 0..1, default 0.7 - 4 pairs of wavy SECONDARY VEINS leaving the midrib toward the tip and
	        curving out to the edge, at least ~1.6 texels wide (thinner ones aliased away; 7 pairs were too fine for the
	        texture): lighter, a sunk groove in the normal map, thicker in the rim map - less sun through them) ·
	        Wrinkle (Leaves: 0..1, default 0.3 - soft noise bumps, ~8 across a leaf, in the normal, the tone and the rim;
	        ~20 read as blotches) · Tilt (Leaves: 0..0.5, default 0.08 - each texture leaf's own random lean in the normal
	        map, per axis; "Trees/Leaf specular curve" multiplies it, and 0.35 lit whole leaves white next to dark ones on
	        one card) · ClusterLeafSize (Needles: the needle
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
	        Dome amount sky (default 0.6 0.4, a Bush 0.6 0: leaves grow to the light - each per-branch leaf / card turned to FACE OUT of
	        the crown and up: the direction from the module root (on the trunk axis, inside the crown) to the leaf / the
	        module length + `sky` x up, jittered; its length direction laid into that dome surface by `amount` (the outward
	        part taken out), so a twig that points out of the crown no longer shows its card's edge. 0 = the cards stick out
	        of their branch. Not the pads. A module does not know its height in the crown, but top modules point up and side
	        modules out, so the directions from its root already form a dome)
	        Keep all inside (default 0.85 0.6: the share of the per-branch leaves kept - `all` everywhere, x `inside` near
	        the module root, the crown's shaded inside, faded to 1 from 0.15 to 0.7 of the module length out; PerBranch is
	        the count before it. Not the pads)
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
	Bake    Variants (baked whole trees per species, 1..64, default 3)
	Billboard (what stands for a baked variant where its meshes do not: "Far representations")
	          Distance (m: the mesh -> VOXELS switch; 0 = the meshes at every distance, casting their own shadow, and
	          no shadow billboard; default 50) · FadeWidth (m, 1..1023, the mesh <-> voxels crossfade band centred on
	          Distance; 10)
	          Resolution (px: the SHADOW BILLBOARD's mask, rounded up to a power of two, 16..2048; 512)
	          TopCardHeight (the shadow billboard's horizontal card, a fraction of the tree's height: 0 = bottom,
	          1 = top; default 0.5 - its bake is orthographic from above, so only the card mesh moves: no re-bake)
	          AlbedoScale (x the voxel grid's colour and the leaf colour a ray sees on the shadow billboard, linear,
	          0..8; default 1)
	          FarAlbedoScale (x the FAR-TREE VOLUME's colour, linear, 0..8; default 1 - with or without `Color Far`)
	Bark    Plates lines rows (1..64 fissure lines per family around / horizontal-break rows along, per tile)
	        Crack (fissure width, ridge fraction) · Breakup (0 continuous lines .. 1 short segments) · Relief · Lichen
	        Lenticels (0..1: birch dashes - the share of a 6 around x 24 along lattice's cells that hold a short dark
	        dash ACROSS the branch) · LenticelSize (0.5..4, default 1: the lattice / this - fewer, larger dashes)
	        Scars (0..1: aspen DIAMONDS - the share of a 3 around x 12 along lattice's cells that hold a dark, rough,
	        sunk diamond wider than tall) · Patches (0..1: about that share of the bark dark, rough patches, wider
	        around than along)
	        Sections (0..1: about that share of the bark tinted to Color Section, soft areas longer along the branch)
	Color   Bark r g b · Leaf r g b · Mark r g b (the lenticels' and patches' colour; default 0.09 0.08 0.07)
	        Section r g b (default 0.45 0.33 0.24) · Twig r g b (the twigs / stems drawn in the LEAF texture; absent =
	        Bark - a birch's white bark read wrong on its twigs) · Far r g b (sRGB like Leaf: the FAR-TREE VOLUME's colour; absent =
	        the leaf texture's alpha-weighted mean, `meanLeafAlbedo`) · LeafVariation (0..3, default 1: the Leaves
	        style's per-leaf spread - toward the autumn tint by up to 0.35 x it, brightness -15 % .. +12 % x it) · Accent
	        r g b + AccentShare (0..1, default 0: that share of the Leaves style's leaves in the accent colour - the odd
	        yellow or red leaf)
	Placement (WORLD placement, TreeWorld; absent or Density 0 = never placed in the world on its own - a Bush then only
        grows as a tree's undergrowth; a Bush's block uses Density, the climate box, Cluster, MaxSlope and Altitude)
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
        Section width thickness [channel] [widthVar] (x the radius, 0.1..4, default 1 1 = round: a flat BLADE - each ring
        an ellipse, `width` across the module's X and `thickness` along its up side +Z, normals from the deformed grid; a
        succulent's leaf. `channel` 0..2: the section bent into a GUTTER, its middle that x the radius below its edges -
        hollow on the up side, keeled below. `widthVar` 0..0.8: a MODULE ROOT's width x (1 +- it), per module - a hash
        of the module's seed, so the random stream is unchanged: leaves of mixed widths) · Swell (-0.9..3: the radius x (1 + swell x sin(pi x the length fraction))
        - a blade widest near its middle instead of a linear taper to a needle) · Tip (0..0.8 of the length narrowing to
        a closed point; default 0 = open end) · Cup (deg, -180..180: a bend over the length toward the module's up side,
        +Z - a blade's face: up for an outer leaf, in toward the centre for an inner one - that stops once it points there;
        a rosette's leaves curving IN like petals. `Curve` bends about a RANDOM axis: leaves that fan out)
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
short whorl (`Crown Top 2 0.25`) carrying a round FOLIAGE PAD from its base (`Leaves Pad 29 0.6 0.1 0.3 0.05`,
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
`Aspen` (a tall, very straight pale grey-green trunk - `Curve 2`, `UpAttract 0.6`; dark diamond `Bark Scars 0.6`, few
large dashes `Lenticels 0.35`, `LenticelSize 2` - bare to 45 % of its height, a NARROW rounded crown (`Radius 3.4`,
`Angle 50 30`) of short ascending limbs whose twigs do not hang (`Droop 10` / `20`), ROUND yellow-green leaves
(`ClusterLeafWidth 0.62`, `Aspect 0.9`, `Color Leaf 0.44 0.45 0.11`) that tremble: `Wind Branch 1.4`; dense even
stands - `Scale 0.95 1.05`, `Spacing 2.5`, `Density 90`: a clone grows from one root system).
Climates: `Oak` Temperate, `Pine` Boreal, `Acacia` Savanna, `Willow` Temperate, `Birch` / `BirchClump` / `Aspen` BirchWood (the bush-matching name; their placement box is a cool
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
(the thin trunk would otherwise cap the module scale) and a short `Billboard Distance` (20-30 m: its voxels take over
there). **Texture budget:** every bush bakes 2 variants (`Bake Variants 2`) from 4 modules (`Module Count 4`; the
succulents 3) - each variant is a voxel grid and a shadow mask, and small plants that look alike need no more.

The SUCCULENTS - `Climate Desert` (no tree's, so never undergrowth), placed ON THEIR OWN by their `Placement` blocks:
`Agave` (a dense FLOWER-SHAPED rosette: 40 slots on a short hidden trunk, the crown from 30 % of its height so the outer
leaves rise off the ground, `Shape Cone` - the outer leaves longest -, `Angle 70 5`; each module ONE broad, short
grey-green blade cupped IN like a petal - `Cup 50`, little random `Curve` -, `Section 1.8 0.3 0.6 0.25` (a gutter on
top, widths +-25 % per module), `Swell 1.0`, `Tip 0.25`, 14 rings x 12 sides, no leaf cards - `Leaves PerBranch 0`;
`Wind Trunk 0`; the desert sand box 20 C up / 0..350 mm, 60 / ha) and `Aloe` (17 LONG, narrower, thicker green blades
rising and curving UP, not fanning out - `Angle 42 15`, `UpAttract 0.5`, `Cup 25`, `Curve 8` -, `Section 1.2 0.5 0.35
0.25`, `Swell 0.4`; pale flecks in bands = the bark's `Lenticels` in a pale `Color Mark`; 18 C up / 50..400 mm,
40 / ha). Both bake 2 variants (`Bake Variants 2`: the plants look alike) from 3 leaf
modules (`Module Count 3`). Having no leaves, they enter the far-tree volume by their BARK blades (`bakeTreeDensity`) in the
bark texture's mean colour.

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

**The GPU uses LEVEL 0 ONLY: no LOD levels for tree pieces** (`uploadLevelZero` uploads level 0 only, as a
one-level `RenderMesh`, and leaves the array's levels above empty). The trees switch through their own tiers (the voxels, the far volume); mesh LOD steps
underneath popped visibly (fewer, larger leaf cards per level, no crossfade). The generator meshes level 0 only
(`MESHED_LODS` = 1 in TreeGenerator.cpp; the table stays for a return of mesh LODs); `lodError` / `Lod ErrorScale`
are unused, and RendererVK allocates no hysteresis slots for tree records.

### The species load

**On JOBS, one Low job per `.tree`** ("Tree species load", `kickLoad` -> `loadSpecies`): the `.tree` parse, the meshes
(the `.treemesh` cache or generated), the textures (`.tex` or generated), the shadow billboards' masks and the voxel
grids (`buildFarTier`) - everything but the GPU objects - into the job's own slot (`SpeciesLoad`), from a snapshot of
the settings (`LoadSettings`). Each `TreeSystem::update` UPLOADS the species whose jobs finished (`pumpLoad` ->
`uploadSpecies`: the baked variants' meshes, the materials, the shadow billboards, the voxel tier, the derived fade materials) and frees that slot's
CPU data at once; once every species is in, the failed ones drop out of `m_species` (name order kept), the fade bands
are written and the trees spawn. Nothing draws while a load runs. `clearAll` (a reload, a disable, the destructor)
joins the jobs first (`stopLoad`).

**The CPU meshes are not kept, and the piece library is never uploaded or cached**: `Species` holds only the GPU
`PieceMeshes` of its baked variants and `volumeAlbedo`. The trunks and the modules are the variant bake's input only:
`loadSpecies` generates them on a `.treemesh` miss and drops them.

### Far representations

A tree draws as its MESHES or as its VOXELS, nothing else. A species with a `Billboard Distance` (> 0) gives every
baked variant two things for where its meshes do not stand (`buildFarTier`, on the species job):

* **THE VOXEL TIER** ("The voxel tier" below): past the species' band - its distance × `Trees/Far distance scale`
  (default 1) - the MAIN pass draws the variant's voxel grid in its meshes' place. With `Trees/Voxel impostors` off the
  meshes draw at every distance. `Trees/Force far` draws every plant as its voxels.
* **THE SHADOW BILLBOARD**: three crossed cards with a mask. It is the variant's sun-shadow caster and rain shelter at
  EVERY distance, and what the rays see of it. It never draws in the main pass.

A species with `Billboard Distance 0` has neither: its meshes draw at every distance, cast their own shadow and (a
tree) get a BLAS.

`treeBakeHash` names the cache files by a bake's INPUTS: the species file's text, the variant's index, `BAKE_VERSION`
and the size - **never by the geometry**: its floats differ in their last bits between Debug and the optimized
builds (`/fp:fast`), which renamed (and re-baked) the cache at every config switch. **A generator change that moves
geometry must bump `BAKE_VERSION`** (TreeImpostor.cpp), or press `Trees/Regenerate textures`.

#### The shadow billboard

* **The cards** (`billboardMesh`, TreeImpostor.cpp): two vertical cards crossed on the trunk axis - seen along X and
  along Z; their PLANES pass through x = 0 / z = 0, not the box centre, so the trunk lands on the crossing - and a
  HORIZONTAL card across the axis at `TopCardHeight` of the box, seen from above (the shadow under a high sun, the rain
  shelter). Both faces of a card read the same strip: a silhouette is the same from both sides. Plain vertices: no
  wind payload, no codes (the shadow VS bends every tree caster by the trunk's sway alone). **Every vertex's tangent =
  the CROWN SHAPE's radii** (the cards' half extents): a ray that shades a hit takes its normal from an ellipsoid in
  the cards' box, not from the flat card (RendererVK Scene.md, `MATERIAL_FLAG_BILLBOARD`).
* **THE MASK** (`bakeBillboardMask`): ONE channel - no albedo, no normal. A small software rasterizer (2×2
  supersampled, back faces culled, the leaf texture's alpha tested) draws each card's view into its strip of one
  square texture. A texel's value (`encodeMaskLevel`): **its COVER up to `TREE_CARD_BARK`** (0.75, RendererVK
  Layout.ixx), and from there to 1 - solid for certain - WHAT it is: `TREE_CARD_BARK` = leaves only .. 1 = bark only,
  by the leaf share of its covered subsamples. So leaves are the cover, clamped, and bark (opaque: its cover is near 1)
  is about its cover too: **the value is continuous across the alpha test (0.5)**, which the shadow pass runs on a
  FILTERED tap. Do not store a solid texel's kind from the test up (RendererVK Scene.md, `MATERIAL_FLAG_BILLBOARD`).
  A ray hit reads the bark share (below). Cache: `<species>_shadowmask<v>_<hash>.tex` (`SHADOW_MASK_VERSION` in its
  key: bump it when the bake's output changes).
* **Strip layout** (`billboardLayout`): 3 strips, each strip's height rounded DOWN to a multiple of the largest power
  of two p that leaves ≥ 4 px per strip at mip log2(p) — so every strip boundary lands on a texel boundary at every
  mip and no mip (and no BC block) mixes two cards. 512 / 3: p = 32, 160 rows a strip, 32 spare rows at the bottom.
  Mips stop at ~4 px per strip.
* **The mips keep the cover**: a level's cover is the mean of the level below (unscaled), its test runs on that cover
  × a scale found by binary search so the SOLID share of the level equals level 0's (as the leaf clusters' mips) - a
  crown casts as much shadow in a far cascade as in a near one. A level's leaf share is the cover-weighted mean.
* **The material** (`createBillboardMaterial`; `PieceMeshes::billboardMaterial`, `MATERIAL_FLAG_BILLBOARD`,
  alpha-tested at 0.5): the mask in the albedo slot - BC4, or R8 without "Compress textures"; RendererVK shows a
  one-channel albedo in all four channels, so every alpha reader (the shadow casters, the ray queries) takes it from
  `.a` as any texture's. The normal slot holds a **2 x 1 texture of the species' two colours for the rays**: the bark
  texture's linear mean, then the leaves' (× `Billboard AlbedoScale`; a leafless species: its bark for both), stored
  as their square roots. A ray that shades a hit (GI, the water's and the film's reflections) mixes the two by the
  mask's bark share (`tree_card.inc.glsl`; an empty or thin texel takes the leaf colour: those rays treat the whole
  card as solid).
* **RT (GI, RT shadows, RTAO, reflections) sees only the shadow billboards** of TREE species: every tree mesh is
  created without a BLAS (`createMesh(..., raytraced = false)`), and BUSHES never get one (their billboard casts in
  the cascades only).

#### The crossfade

A band `FadeWidth` × `Trees/Fade width scale` wide (default 2; clamped to 1..1023 m, the material's 10 bits; respawns -
the GPU set keeps the width per type), centred on the species' distance × `Far distance scale`. `Trees/Voxel start
(m)` (default 0 = off; respawns) sets where a TREE's band STARTS, in metres, for every tree species (bushes keep their
own): `farDistance` stores it unscaled, (start + width / 2) / `Far distance scale`, so the culls and the fade bands,
which apply the scale, land it there - and a `Far distance scale` change respawns while it is on. Inside the band
(centre distance ± the piece radius) the tree draws its meshes as `barkFade` / `leavesFade` — the same meshes on
`LitTree` with species materials DERIVED with a distance fade-OUT (`RenderMaterial::derive`, shared textures) — plus
its voxels, whose material fades IN over the same band. The tree FSs' dither (tree_fade.inc.glsl, RendererVK
`MATERIAL_FLAG_DISTANCE_FADE`) gives each pixel to exactly one of them; TAA blends the per-frame dither. Outside the
band only one side draws, the meshes on their normal (early-depth) materials. `applyFadeBands` writes the bands into
the material flags (at reload and when the distance scale changes — no reload). The shadow pass has no fade: the
shadow billboard casts at every distance. `Force far` keeps the hard switch.

### The voxel tier (`Trees/Voxel impostors`)

On by default, for every species with a `Billboard Distance` - trees and BUSHES. Past the species' band the MAIN pass
draws a baked variant's PREFILTERED VOXEL GRID, marched per pixel (RendererVK `LitTreeVoxel`, Scene.md
"THE VOXEL TIER"), up to the hand-over to the far-tree volume. With "Trees/Voxel
shadows" (off, live) the grid is also the tree's sun-shadow caster at every distance (Scene.md "THE VOXEL SHADOW
CASTERS"); the rays keep the shadow billboard. Past the world set's near chunks the renderer draws the
records' trees as voxels too ("Trees/Voxel far trees", RendererVK Scene.md "THE FAR VOXEL TREES"): `updateWorld` hands it
the set's chunk coordinates every frame (trees only: `RenderTreeSet::Type::farRing` is off for a bush). "Trees/Voxel
resolution" (32 / 64 / 128 / **256** voxels per axis at level 0 for a TREE; a BUSH takes a quarter, at least 32: it is
about a metre, and its band starts 20-30 m out) and the toggle both reload. 256: its bake holds ~1 GB of level-0 grids on the CPU while it runs (16.7 M voxels,
leaves and bark apart). A bake logs "Trees: '<species>' voxels <v> at <res>: <stored> of <all> bricks stored (%), MB".

* **The bake** (`bakeTreeVoxels`, TreeImpostor.cpp; in `buildFarTier`, on the species job): over `treeVoxelBox` (the
  tree's box + 1.5 voxels each side), the LOD-0 leaves and bark splatted by PROJECTED area per point of a barycentric
  grid (half a voxel apart: up to 1024 points an edge - with a low limit the points of a large trunk triangle stand
  further apart than a voxel of the 256 grid), each point's weight x the texture's ALPHA there and its colour the
  texture's (linear):
  a leaf counts half its area (two-sided, random facing), the bark half too (a quarter - a convex body's mean - left
  the trunks see-through: a surface far below a voxel thick is not spread evenly through it). The leaf normals are
  turned out of the crown (from the leaves' box centre). Per voxel: OPACITY = `1 - exp(-area / crossing^2)` for one
  crossing (the cube root of the voxel's volume), leaves and bark alike. **THE BARK BODIES ARE SOLID**: that law is for
  a cloud of small surfaces, and one bark WALL that crosses a whole voxel gets only ~0.4 from it - a trunk is a hollow
  shell of two such walls, so a ray through it passed in ~40 % of the pixels (worst at a fine grid, where the walls are
  many voxels apart). Where a voxel's bark normals AGREE (mean length >= 0.93: a wall of a body at least ~2.5 voxels
  thick; around a thinner twig they cancel), the voxel one step BEHIND the wall, along the normal, becomes opaque (1)
  with the wall's colour and normal. The wall voxels keep their partial opacity, so the silhouette does not grow. Then
  the mean colour and the MEAN normal (short where
  the leaves face every way), the bark weighted by what the voxel's own leaves leave of it, `exp(-leaf area /
  crossing^2)` - by area alone the branches inside the crown greyed it
  (a bark-only voxel, the trunk under the crown, keeps the bark) - then x `Billboard AlbedoScale`; and the INTERIOR = the smallest optical depth to the box's side along the six axes, over the tree's largest.
  Colour and normal are dilated two rings into the empty voxels (the filter would pull a surface voxel toward black).
* **The levels** halve down to 4 voxels: a coarse voxel's opacity is 1 - the MEAN TRANSMISSION through its children along
  the three axes (12 two-voxel columns), not the mean opacity, so a dense clump keeps its cover far away; the rest
  opacity-weighted.
* **SPARSE BRICKS** (TreeImpostor.ixx "The VOXEL tier", `tree_voxel.inc.glsl`): the grid is NOT stored whole - most of
  a tree's box is air. Three textures per variant, ONE level each (no mip chain: every level's bricks sit in the same
  atlases and take what they need):
  * A level of n voxels is cut into BRICKS of 7 voxels a side, CENTRE TO CENTRE: brick k covers the voxel centres
    7k .. 7k + 7 per axis, so it stores 8 voxels a side and a filtered sample inside it never reads another brick (the
    shared face is stored by both - a one-sided border, x 1.49; a border on both sides of 6 voxels cost x 2.37). A
    brick whose 8^3 voxels hold no opacity is not stored.
  * **The opacity atlas** (one channel: BC4, R8 without "Compress textures"): every stored brick of every level, slot
    s at column s % cols, row s / cols, as its 8 z slices side by side (64 x 8 texels: whole BC blocks). Slot 0 is the
    HEADER, no brick.
  * **The surface atlas** (RGBA, linear: BC3 / RGBA8), half the opacity atlas each way: per stored brick 4^3 samples
    over the same span (one every 2.33 voxels - the colour and the normal change slowly, the opacity carries the
    silhouette), as two tile sets side by side: RGB the mean colour (sRGB-ENCODED: the FS decodes it, with a cubic fit of the curve - at most 0.002 off, no pow) + A the interior,
    then RGB the mean normal x 0.5 + 0.5 + A the LEAF SHARE (the leaves' part of the voxel's seen surface: 1 = leaves
    only, 0 = bark only - the trunk; an empty voxel counts as leaves). The shader's leaf-only terms take it. A sample
    is the opacity-weighted mean of the voxels under a tent of the sample spacing.
  * **The index** (RGBA8, exact): texel (0, 0) the header (log2 res, the levels, log2 cols); below it level by level
    its D^3 bricks (D = `treeVoxelBricks(n)` = ceil((n - 1) / 7)): brick (x, y, z) at (x + z D, y), RGB = its slot,
    0 = EMPTY. At 256: 1369 x 76.
  * Two `.tex` files per variant: `<species>_treevoxel<v>r<res>_<hash>.tex` (the opacity atlas as its albedo, the
    surface atlas as its normal) and `..._index.tex`, keyed as the shadow masks are plus `TREE_VOXEL_BAKE_VERSION` (bump
    it when the bake changes). The box is rebuilt from the geometry on every load.
* **The material** (`createVoxelMaterial`; `PieceMeshes::voxelMaterial`, `MATERIAL_FLAG_LEAF`): the opacity atlas in the
  albedo slot, the surface atlas in the normal slot (`TextureDesc::normalWidth / normalHeight`: its own size); the index
  is a bare linear `RenderTexture` (`PieceMeshes::voxelIndex`). **The opacity atlas's header carries the index texture's
  bindless id** (its first two 4 x 4 blocks = the id's low and high byte, written at upload: a BC4 block of two equal
  endpoints decodes exactly): the shadow casters get ONE texture index (the cull's alpha index), and both passes find
  the index through it (`voxelGrid`). It takes the band's fade-in in `applyFadeBands`; the box mesh
  (`treeVoxelBoxMesh`) has no BLAS.
* Shading tweaks ("Trees"): "Voxel LOD bias" (+1 = voxels twice the pixel size, half the steps), "Voxel sharpness" (0..0.95,
  default 0.95: the filter's REACH - the bake writes every surface point into its one voxel, but trilinear sampling spreads
  each voxel's opacity a voxel into its neighbours, fuller clumps and silhouette; `voxelSharpen` steepens the fraction
  between texel centres by 1 / (1 - this), toward each voxel covering its own cell - every opacity tap: the view, the
  self-shadow and the shadow casters), "Voxel thinning" (0..0.95,
  default 0: every march's sampled opacity with its extinction x (1 - this), `voxelThin` - the view, the sun self-shadow
  and the shadow casters alike; a crown fuller than its mesh thinned to match), "Voxel self shadow",
  "Voxel normal hull", "Voxel saturation" (about the linear luminance: the bake's averages - the twigs drawn in the leaf
  texture, a sparse voxel's branches - read greyer than the mesh leaves), "Voxel brightness" (x the linear colour, after
  the saturation), "Voxel transmission" / "... glow" / "... focus" (x the "Foliage transmission" ones) and "Voxel
  transmission self shadow" (the share of the voxel self-shadow the transmitted sun takes; in place of "Foliage
  transmission self shadow"); the other "Foliage ..." leaf terms apply as on the mesh leaves.

### Texture files (`Assets/Local/Trees/Textures`)

Generated output under `Assets/Local`, so NOT in git: a fresh checkout generates them on the first load.
**Each material is ONE `.tex` file AS UPLOADED** (MeshCache's `CachedTexture`, see "The generated-mesh cache" in
[CONTEXT.md](CONTEXT.md)): the albedo and normal mip chains in their GPU encoding plus the albedo's linear mean (the
far volume's colour), zstd. A load is one read and one decompress - no image decode, no mip build, no BC encode.
`<species>_bark.tex`, `<species>_leaves.tex` (cluster species only; its normal map: the leaves' tangent-space normals -
cupped, tilted leaves (`Leaves Cup`) and round twigs, drawn with the same coverage as the colour; A = the leaf's RIM, 0
at the midrib .. 1 at the edge, 0 on twigs - LitTree's transmission thickness; BC3 on the GPU for the A), plus the
shadow masks and the voxel grids `<stem>.tex`. Keyed inside by the species file's text, `Compress textures` and the
size, so a changed `.tree` regenerates them; `Trees/Regenerate textures` overwrites all. Writing a `.tex` removes the
level-0 PNGs it replaces. **A `.tex` holds no RGBA8 level 0**: a mask or voxel bake miss regenerates the bark / leaf
images it needs (`buildFarTier`; the generators are deterministic). The rocks' DEAD WOOD keeps its own tinted bark
`.tex` ([Rocks.md](Rocks.md)).

**GPU formats** (`Trees/Compress textures`, default on, reloads; a change is a `.tex` miss, so it re-bakes the
masks and the voxel grids): the finished mip chains are BC-compressed when generated
(File `TextureConvert::compressBlockRows`, stb_dxt, every level's block rows a `parallelFor` task, "Tree texture BC"):

| Texture | Format |
|---|---|
| bark albedo | BC1 sRGB (opaque) |
| bark normal | BC5 (XY; MATERIAL_FLAG_BC5_NORMAL rebuilds Z) |
| leaf cluster atlas | BC3 sRGB (alpha-tested) |
| shadow billboard mask | BC4 (one channel: cover, then the bark share) |
| voxel opacity atlas / surface atlas | BC4 / BC3 linear |

4x less VRAM than RGBA8 (BC1 and BC4: 8x for their one or three channels). Off = the uncompressed chains (the A/B for
compression artefacts).

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
ACROSS the branch (`lenticel`: one per lattice cell by chance, an ellipse, slightly sunk; `LenticelSize` divides the
lattice in whole cells, so it still tiles), aspen SCARS (`scar`: a diamond - the L1 ball - per cell by chance, its
outline roughened by the grain) and dark patches (a
thresholded fbm, 2 cells around x 4 along, the grain deepened in them). SECTIONS (`Sections`, in `Color Section`,
under the marks): soft areas of another tint (a thresholded fbm, 3 around x 2 along: longer along the branch) - a
birch's brown older bark. sRGB albedo plus a LINEAR RGB tangent-space normal map from the
same height field (x along u, y along v — the tube's tangent / bitangent), each with a CPU box-filtered mip chain
(normals renormalized). Uploaded through `RenderMaterial::createTextured` (with `normalMips`), freed with the species.

### Leaf types

* **`Single`** — one double-sided diamond per leaf, the species' solid leaf colour, `LitOpaque`.
* **`Cluster`** — alpha-tested CARDS (`LitTree`), each showing a twig with `ClusterLeaves` leaves. `Size` /
  `Aspect` are then the card's, `PerBranch` counts cards, and a card's stem starts at the branch centre (the
  texture's twig grows out of the wood). `Cross true` adds a second card at 90° about the stem axis.
  **Both faces are separate triangles** (back-face culled); the BACK face's tangent and bitangent are the front's
  NEGATED (`appendCard` / `appendLeaf`), so its normal-map normal is exactly the front's negated - the same leaf from
  below, its cup convex. (The front's frame on the back mirrored the surface through the card: concave from both sides.)
  The texture is PROCEDURAL until authored textures exist (`TreeLeafTexture.cpp`): per species a 1024² 2×2
  atlas of cluster variants (stem at v = 0, each card picks a cell), sRGB colours from `Color Leaf` / `Bark`,
  and its OWN mip chain — every level's alpha is rescaled (binary search) so the fraction of texels passing
  `TREE_LEAF_ALPHA_CUTOFF` (0.5) matches level 0, otherwise the crown thins with distance. Uploaded through
  `RenderMaterial::createTextured`, freed with the species (its `RenderMaterial`s, `TreeSystem::clearAll`). Every leaf / needle / shoot /
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

### Baked tree variants (what a record expands to)

The runtime does not composite pieces per tree: at load every species bakes `Bake Variants` (3) whole trees
(`bakeTreeVariant`: the composite of a seed, all its pieces' bark merged into one mesh and all their leaves
into another, PER LOD LEVEL, at scale 1; each level's error = the largest piece error × its placement scale).
A variant is a `TreePiece` like the library's (`SpeciesLoad::variants` -> `Species::variantMeshes`), with its voxel
grid and its shadow billboard ("Far representations"). The world places one variant per tree with a seeded variant,
scale (`Scale` × `Size variation`: 2^±v, log-uniform, default 0.6 = ×0.66..1.52) and yaw. The library's pieces are the
bake's input only: they are never uploaded, and the `.treemesh` cache holds the variants alone.

**The WIND payload** (RendererVK "Tree wind", `tree_wind.inc.glsl` for the bit layout): `meshPiece` writes every bark
and leaf vertex's `texCoords.z = 1 + payload / 2^22` (`windPayload`): the module weight along the module's root bone
(a sub-branch's vertices take it at their bone's pivot), the level-1 sub-branch weight along its bone, the bone's phase,
a leaf card's TIP flag (corners 2 / 3; a diamond's all but the stem) - a card's weights from its STEM, so all four
corners share one phase. Trunk pieces: the wind bit only (the trunk bend). `bakeTreeVariant` ORs each module
placement's phase (3 bits) into its vertices (two placements of one module sway apart). No
vertex grows: the magnitude of the tangent's w was free. The species' `Wind` scales ride its piece TYPES
(`RenderTreeSet::Type::windTrunk / windBranch`, RendererVK "Tree wind"): the cedar (large, heavy limbs in the trunk
piece) `Trunk 0.4`, `Branch 0.7`.

### The tree set

Every tree draws through ONE RendererVK tree instance set (`RenderTreeSet::create`, held in `m_treeSet`, built by
`spawnWorld`; `destroyTreeSet` destroys it before the species' meshes): a piece TYPE per baked variant
(`buildGpuTypes`: its bark and leaves, their derived fade-out materials, its voxel box and the band, its shadow
billboard; `farRing` off for a bush), then CHUNKS of
placed pieces (transform, far centre + radius, type), added and removed as the camera moves ("World mode"). A chunk is
uploaded once to device-local memory, and the culls make the per-piece decision and build the records themselves, and
the TLAS writer their RT instances (see RendererVK "BAKED TREE RECORDS"). **The pieces live in the TERRAIN CHUNKS:**
one set chunk per terrain chunk (`Globals::terrain.chunkSize()`; a chunk-size change respawns), and the set is hooked
into the terrain (`hookTerrain` -> `TerrainStreamer::setVegetation`,
[TerrainStreamer.md](TerrainStreamer.md)): the terrain's render walk lists the chunks it draws with their passes, and
its sink calls `m_treeSet.render` with that list - so the culls only see the plants of the visible chunks (all
passes) and of the shadow / GI sphere's chunks (shadow + GI). **The sink runs on the walk's worker: it reads the band
settings through atomics.** `update` binds the set (`bind`, main) and submits EVERY chunk itself when
the walk does not carry it (no walk this frame, or the respawn frame - the new hooks start with the next walk).
Unhooking (`destroyTreeSet`) joins the walk.

Every baked variant also carries its FAR-TREE VOLUME grid (`bakeTreeDensity`, `TREE_DENSITY_RES` = 32³ extinction
over its box, from the LOD-0 LEAF triangles only; cluster leaves count half opaque. No bark: a trunk's whole
area in one thin voxel column draws dashed vertical streaks through the far blobs - EXCEPT for a piece with no leaves at
all (the succulents' bark blades), which splats its bark at a quarter of its surface) and the species leaf colour — the
leaf texture's alpha-weighted mean, DECODED FROM sRGB (`meanLeafAlbedo`; the albedo textures upload as sRGB, so the
raw `leafColor` taken as linear reads light yellow-green; a leafless species: its BARK texture's mean) — handed to the set as the type's density / albedo
(RendererVK "Far-tree volume"). The SHADOW BILLBOARD is the piece's only shadow / GI / RT representation at
every distance: the meshes and the voxels draw in the MAIN pass only. The set is destroyed on respawn / reload / disable.

### World records (`TreeWorld`, "Trees/World" tweaks)

Every tree of the terrain ring as a **4-byte record per terrain chunk** (`TreeRecord`: x, z 12 bits each, chunk-local
on a `TREE_RECORD_STEPS` = 4096 lattice; the type 8 bits = its index in the NAME-SORTED `.tree` list, then the
`.rock` list). No height (the terrain's), no variant / scale / yaw: `treeRecordSeed(seed, chunk, record)` hashes the
chunk and the QUANTIZED position, never the record's index, so a change elsewhere in the chunk leaves a tree as it
was. **Mirrored in `Assets/Shaders/Trees/tree_record.inc.glsl` - keep in step.** A member of TreeSystem, updated
before its `Enabled` check: it runs with "Trees/Enabled" off.

* **Ring:** the terrain's (`ringRadius()`, `chunkSize()`, `generatedBounds()`), but **no farther than the far-tree volume
  draws** ("Trees/Far end", 15 km, + one chunk: a record past it serves nothing - the terrain's ring reaches ~33 km, ~7x
  the chunks) and at least one past the keep radius; one chunk of hysteresis on eviction. A "Far end" change is a ring
  change: a restart.
  On a camera chunk change the missing chunks are requested; Low pump jobs (`Gen jobs`; 0, the default = one per job
  worker - `pumpCap`; they yield at every preemption point, so they only fill idle workers) take the front of the READY
  queue, with lazy staleness as the terrain pumps.
* **THE CHUNKS FOLLOW THE TERRAIN NODES** (`TerrainStreamer::setNodeHook`, set in `initialize`): a requested chunk WAITS
  (`m_waiting`) for the terrain node over it. Right after the terrain pump generates a node it calls the hook
  (`onTerrainNode`, on that pump): the node's waiting chunks move to the ready queue together, row by row - the pumps
  then place them while the node's tiles are still in the RAM cache, so the tile working set is a node's area, not the
  whole perimeter of the ring (the nearest-first order sweeps it). A LOD0 node places its chunk AT ONCE
  on its own pump, reading the node's 2 m sample grid for the 8 m placement grid's points it holds (`sampleField`: every
  4th sample; the halo in up to four strips) instead of sampling them again. That reuse is exact - whole-metre
  lattices, the very doubles the full grid would sample, the samplers pure functions of position - so a chunk gives the
  same records however it was placed (a keep-radius regeneration must match the records the GPU holds). A chunk goes
  straight to the ready queue when its node was generated already (`TerrainStreamer::nodeGenerated`: the streamer
  records a node's base chunks BEFORE calling the hook, and TreeWorld checks under its lock, so no chunk falls between)
  or when it is a keep-radius regeneration (`Request::regenerate`). **The waiting ones left when the terrain pumps go
  idle become ready too** (`promoteWaiting`, nearest first, in the bookkeeping job): a node that stays resident is not
  generated again. The hook is guarded by a token (`HookToken`): the procedural globals' teardown order is undefined.
* **THE BOOKKEEPING JOB** (`"TreeWorld update"`, Normal, `m_updateCounter`): merging the pumps' results, the ring
  rescan (a walk of every chunk + the (2R+1)² ring) and the next upload pick run OFF main. main.cpp kicks it
  (`TreeSystem::kickWorldUpdate`) right after `clutter.update` - the LAST reader of the records this frame
  (`cpuRecords`) - and the next `TreeWorld::update` joins it first, **so nothing reads the chunks while it runs**.
  (The game's harvest survey reads them in the same window, right before the kick: `TreeSystem::surveyHarvest` - one
  visit per tree and rock within a circle, with its world position, its wood and its stone, by `FloorType::harvest`; no answer when the CPU does not hold a
  chunk the circle touches.) It
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
  `Climate sharpness` (4). The candidate exists with probability (the densities' SELF-WEIGHTED MEAN: sum d^2 / sum d)
  x the cell's hectares x `Density scale` (so at most one tree per cell: 400 / ha at 5 m); its species is picked by
  share. **OVERLAPS KEEP THE DENSITY**: one species alone keeps its own, two fitting boxes about their mean (an
  oak-birch band no longer grows ~2x the trees), and a rare species in a dense one's box (Willow, 1 / ha, in Oak's
  90) takes almost nothing away from it - the dense one's density dominates the mean. (The SUM doubled the trees in
  every overlap; picking by Density x fit let a dense species' tail outnumber a sparse one's core; normalizing by
  the sum halved an equal-fit species.) **GRASSLANDS** ("Grassland share", 0.5; "Grassland size (m)", 800): a world fbm (3 octaves at
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
  any spacing the chunks place what they did before. **FOREST TYPES** ("Forest type size (m)", 600; "Forest minority", 0.05): one species DOMINATES a
  stand - each species has a large-scale noise (its own seed, 2 octaves at that size); of the COMPETING species at a
  point (those with at least 0.25 x the densest one's density there) the highest noise wins, the others x the minority
  factor (fully once 0.06 of the noise behind the winner: a soft border between stands), then the competitors rescaled
  to their own total - which trees grow changes, not how many (birch forests among the oaks' and pines' edges, not
  every species in every wood; 1 = off). A RARE species (Willow, 1 / ha, among Oak's 90) does not compete: it keeps its
  own density everywhere (competing, a won willow stand cut the oaks to the minority, and the rescale made ~1 tree in
  6 a willow). A tree's undergrowth bushes are not stored. No tree, bush or rock where the river influence is above
  "Terrain/Rivers/Vegetation clear" (except rock rules with a River term, below).
* **THE BUSHES ON THEIR OWN** (a `Kind Bush` species with a `Placement` block - the desert's `Agave` / `Aloe`):
  records of their own, from a pass AFTER the trees on their own chunk lattice (`BUSH_CELL` 3 m, at most one per cell,
  hash salt 0xB054), their densities ADDED (independent ground covers; one picked by share). Each one's climate fit is
  a tree's (the box, the Gaussian outside it, the fade) x its `Cluster` patches, gated by `MaxSlope` and `Altitude` -
  but NONE of the trees' grassland mask, sharpening or forest types: a bush needs no forest. Out of the rocks'
  footprints (the trees' test) and `BUSH_TREE_CLEAR` (2 m) from this chunk's trees (a tree's undergrowth is its own).
  The record expands to the bush alone (`expandChunk`: a bush species has no undergrowth); its floor type is a tree's
  (kind 1: its crown and trunk radius).
  **The existence roll goes FIRST** (exact, the same result): a tree candidate whose roll is above the species'
  summed `Density` x the cell's hectares, or a rock cell whose roll is above its type's bound (every rule's `Density`
  x its largest ground / forest / river / flow multipliers - every fit term is at most 1), skips its field sample and
  fits. The river grid (1 m) covers only what is read: the chunk plus the neighbours' rock reach + 2 m, not the
  terrain halo. A species (or bush) whose climate distance puts its Gaussian under "Climate fade start" skips its `exp`
  (`fitCut`: its fit is exactly 0 there), the grassland noises run only where some species fits, and the bushes' tree
  clearance looks up the tree per candidate cell around the bush (`treeAtCell`), not every tree of the chunk - all exact.
* **A new generation** (everything dropped, every chunk regenerated) on: Enabled / Seed / cell / density / climate /
  grassland / forest type tweaks, `Reload species`, a new sampler, a chunk size or bounds change, a rock change (`setRocks`). The generation
  counter is monotonic, so a late pump result of an old config never merges.
* **GPU:** each merged chunk joins an upload queue; `Upload KB per frame` (1024) of records per frame go to the
  renderer's pool (`Renderer::addTreeRecordChunk`, RendererVK "WORLD TREE RECORDS"; `GPU pool (MB)`, 128, fixed - a
  full pool refuses chunks and warns once; a change drains the GPU and regenerates), each with its GROUND:
  `placeChunk` also writes the chunk's `TREE_RECORD_GROUND_RES`² = 17² heights (16 m at 256 m chunks, bilinear in its
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
  600..2200 mm, 90 / ha; Birch 4..8 C / 600..1800 mm, 80 / ha, BirchClump the same, 50 / ha (the cool band between
  Pine and Oak); Oak 6..18 C / 750..2200 mm, 90 / ha, `Cluster 60 0.8` (it shares 6..8 C with the birches - Forest
  types split that band into oak and birch stands; birch-only bands were the most common forest); Aspen 2..5 C /
  450..1400 mm, 90 / ha (the birches' bushes, `BirchWood`; the COLD, DRY side of the birch band: its own 2..4 C next
  to Pine, aspen and birch stands in 4..5 C, the birches alone in 5..6 C); Willow 11..18 C / 1300..2200 mm, 1 / ha (the wettest of Oak's);
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

`Trees/Enabled` + `Trees/World/Enabled` + the terrain: the trees are TreeWorld's records. Without TreeWorld or the
terrain there is no set and no tree draws (the species stay loaded). Switching it, a TreeWorld restart (its
`generation()`) or the `Trees/Respawn` button builds the set again.

* **One set** (RendererVK `RenderTreeSet::create`: every piece type, a fixed `Trees/World/Set
  capacity (pieces)` of slots, 600 k; no growth, a full set refuses chunks and warns once), hooked into the terrain
  (`hookTerrain`; chunk index capacity (2 x near + 3)² x 2).
* **`Trees/World/Near radius (chunks)`** (3, Chebyshev): every chunk inside it that TreeWorld holds CPU records for
  (TreeWorld's keep radius is raised to near + 1: `requireKeepRadius`) is EXPANDED on a Low job, nearest ring first,
  at most 4 in flight: per record its tree (`treeRecordSeed`: the variant, the scale - the species' `Scale` range x
  `Size variation` - and the yaw from the seed, `placeVariant`) on the ground of ONE Full `sampleGrid` at 2 m (the
  terrain's LOD 0 spacing) over the chunk plus the bushes' reach, plus its BUSHES (its species' `Undergrowth`, else
  `Bushes per tree` - default 4, the fraction by chance - out to 8.25 m; 1.5 m .. the radius out, area-uniform; the
  bush species of its climate by `Weight`, with the same variant / scale / yaw rules; `ExpandSpecies::bushesPerTree` /
  `bushRadius`, `bushUndergrowth`), none in WATER (at the grid's nearest point: under a river's / lake's surface or the sea, or a
  river bed past "Terrain/Rivers/Vegetation clear"; the far volume's GPU mirror has no water data and keeps them).
  **The job reads only an immutable `ExpandContext`** (types, scales, far centre / radius per variant, the record-type
  -> species map by name, the sampler) and a copy of the records.
* `Expand per frame` (2) finished chunks join the set per frame (`addChunk`), the coordinate is mapped
  (`m_vegChunkOf`) and re-stamped in the terrain (`restampVegetation`). A chunk past near + 1 is removed
  (`removeChunk`, deferred reuse) and re-stamped; a pending one's result is dropped.
* A respawn joins the jobs (`stopExpansion`: the generation bumps, the results are cleared).
* **THE COLLIDERS** ("Trees/World/Colliders": `Enabled`, `Radius (m)` 80, `Bodies per frame` 64): the expansion also
  lists per chunk what gets a STATIC body (`ExpandCollider`: the piece's transform) - every TREE (not the bushes: theirs
  is empty) its species' trunk as an 8-sided cylinder HULL (`ExpandSpecies::colliderPoints`: the base radius with half
  the flare, from 0.3 m under the ground to the mean trunk height, at scale 1; the body's scale is the tree's), every
  ROCK its LOD chain's LAST level as a MESH (`m_rockColliderMeshes`: one BVH per variant, built by `spawnWorld` from
  `RockSystem::WorldVariant::collisionPositions` / `Indices` - TreeSystem's own copy, so RockSystem may free its meshes
  first). Layer "Vegetation". `updateColliders` (main, at the end of `updateWorld`) creates the bodies within the radius
  of the NEAREST FOCUS POINT (`setColliderFocus`, up to 8 a frame: the App passes the terrain collider's points - the
  co-op game's players and Base; none = the camera) (horizontal; at most `Bodies per frame`) and destroys those 10 %
  past it; a chunk wholly past it drops
  all of its bodies, a chunk leaving the near radius takes them along, and the tweak off drops every body. Only the near
  chunks' trees ever have one - millions of records, a few hundred bodies. box3d create / destroy serialize on
  `g_bodyLifecycleMutex`: the terrain collider's update job may create its tiles at the same time. `destroyTreeSet`
  (a respawn, RockSystem's mesh-user hook) drops the bodies before the rock meshes.
* **THE WORLD'S ROCKS**: `spawnWorld` appends RockSystem's loaded types (`Globals::rocks.worldTypes()`) to the set - a
  GPU type per variant: ONE mesh, the LOD chain's level 0 in the `bark` slot on `LitRock`, no voxels and no shadow billboard (RendererVK:
  the culls pick its level, no wind) - and maps TreeWorld's rock record types to them by name. `expandChunk` turns a
  rock record into ONE piece (`placeRock`): variant and yaw from the seed as a tree's, the scale from
  `rockRecordScale`, laid ON THE GROUND by `RockSystem::groundTransform` (Rocks, "A rock on the ground"
  ([Rocks.md](Rocks.md)): it follows the ground's normal by `Align`). The set's types are fixed, so RockSystem's
  `worldGeneration()` (its meshes arrived, changed or went - it loads in the background) respawns the set; and
  **before RockSystem frees its meshes it calls TreeSystem's mesh-user hook (`setMeshUser`), which destroys the set.**
  **ROCKS IN THE FAR VOLUME**: rock meshes hand over to the far-tree volume at its start as a tree's voxels
  (RendererVK tree_cull.inc.glsl: every type drops out of the main pass past `Far start` + `Far overlap`; a rock keeps
  casting its sun shadow). A rock GPU type carries its variant's OCCUPANCY grid
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

### Loading and drawing

`Trees/Enabled` loads every species (enable / `Reload species` re-reads the files — no hot reload). Bark draws with
the species' bark texture on `LitOpaque`; leaves on `LitTree` (cluster cards) or a solid colour on `LitOpaque`
(`Single`). `Bush shadow distance (m)` (default 100, 0 = no limit): bushes farther than this from the shadow
cascades' centre cast no sun shadow (`RenderTreeSet::Type::shadowDistance`).
